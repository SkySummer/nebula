#include "nebula/server/http_sub_reactor.hpp"

#include <cerrno>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"

namespace nebula::server {

void HttpSubReactor::close_connection(int fd, std::string_view close_reason) {
    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    Connection connection = std::move(it->second);
    connections_.erase(it);

    if (!epoll_.del(fd)) {
        const int err = errno;
        common::Logger::instance()
            .warn("sub reactor epoll del connection failed")
            .field("fd", fd)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("decision", "continue_close");
    }

    common::close_fd(fd);
    connection_count_.fetch_sub(1);

    common::Logger::instance()
        .debug("sub reactor connection closed")
        .field("fd", fd)
        .field("peer", connection.peer)
        .field("reactor_id", id_)
        .field("close_reason", close_reason);
}

void HttpSubReactor::sweep_idle_connections() {
    if (config_.read_timeout.count() <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<int, std::string>> idle_connections;
    idle_connections.reserve(connections_.size());

    for (const auto& [fd, connection] : connections_) {
        if (connection.processing || !connection.write_buffer.empty()) {
            continue;
        }

        const auto idle_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - connection.last_active);
        if (idle_duration > config_.read_timeout) {
            idle_connections.emplace_back(fd, connection.peer);
        }
    }

    for (const auto& [fd, peer] : idle_connections) {
        common::Logger::instance()
            .info("sub reactor idle connection timeout")
            .field("fd", fd)
            .field("peer", peer)
            .field("reactor_id", id_)
            .field("timeout_ms", config_.read_timeout.count());
        close_connection(fd, "idle_timeout");
    }
}

std::size_t HttpSubReactor::close_idle_connections_for_shutdown() {
    std::vector<int> idle_fds;
    idle_fds.reserve(connections_.size());

    for (auto& [fd, connection] : connections_) {
        connection.close_after_write = true;
        if (!connection.processing && connection.write_buffer.empty()) {
            idle_fds.push_back(connection.fd);
        }
    }

    for (int fd : idle_fds) {
        close_connection(fd, "graceful_shutdown");
    }
    return idle_fds.size();
}

}  // namespace nebula::server
