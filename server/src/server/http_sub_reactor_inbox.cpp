#include "nebula/server/http_sub_reactor.hpp"

#include <cerrno>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/server/reactor_constants.hpp"

namespace nebula::server {

bool HttpSubReactor::enqueue_accept(AcceptedConnection accepted) {
    {
        std::lock_guard lock(pending_accepts_mutex_);
        if (run_state_.load() != RunState::Running) {
            return false;
        }
        pending_accepts_.push_back(std::move(accepted));
        pending_accept_count_.fetch_add(1);
    }
    notify_wakeup();
    return true;
}

bool HttpSubReactor::enqueue_response(ReactorResponseTask response) {
    {
        std::lock_guard lock(pending_responses_mutex_);
        if (run_state_.load() != RunState::Running) {
            return false;
        }
        pending_responses_.push_back(std::move(response));
        pending_response_count_.fetch_add(1);
    }
    notify_wakeup();
    return true;
}

void HttpSubReactor::drain_pending_accepts() {
    std::vector<AcceptedConnection> pending_accepts;
    {
        std::lock_guard lock(pending_accepts_mutex_);
        if (pending_accepts_.empty()) {
            return;
        }
        pending_accepts.swap(pending_accepts_);
    }

    pending_accept_count_.fetch_sub(pending_accepts.size());

    for (AcceptedConnection& pending : pending_accepts) {
        if (current_lifecycle_state() != LifecycleState::Running) {
            common::close_fd(pending.fd);
            continue;
        }

        if (!epoll_.add(pending.fd, kConnectionReadEvents)) {
            const int err = errno;
            common::Logger::instance()
                .error(common::LogDomain::Server, "sub reactor epoll add connection failed")
                .field("fd", pending.fd)
                .field("reactor_id", id_)
                .field("errno", err, common::errno_message(err))
                .field("next_state", "closing");
            common::close_fd(pending.fd);
            continue;
        }

        Connection connection;
        connection.fd = pending.fd;
        connection.token = pending.token;
        connection.peer = std::move(pending.peer);
        connection.last_active = std::chrono::steady_clock::now();
        const std::string peer = connection.peer;
        connections_.emplace(pending.fd, std::move(connection));
        connection_count_.fetch_add(1);

        common::Logger::instance()
            .debug(common::LogDomain::Server, "sub reactor connection accepted")
            .field("fd", pending.fd)
            .field("peer", peer)
            .field("reactor_id", id_);
    }
}

void HttpSubReactor::drain_pending_responses() {
    std::vector<PendingResponse> local_responses;
    {
        std::lock_guard lock(pending_responses_mutex_);
        if (pending_responses_.empty()) {
            return;
        }
        local_responses.swap(pending_responses_);
    }

    pending_response_count_.fetch_sub(local_responses.size());

    for (PendingResponse& item : local_responses) {
        const auto it = connections_.find(item.fd);
        if (it == connections_.end()) {
            continue;
        }
        if (it->second.token != item.connection_token) {
            common::Logger::instance()
                .debug(common::LogDomain::Server, "sub reactor stale pending response ignored")
                .field("fd", item.fd)
                .field("reactor_id", id_)
                .field("pending_connection_token", item.connection_token)
                .field("active_connection_token", it->second.token);
            continue;
        }
        apply_response_to_connection(it->second, item.response, item.close_after_write, item.suppress_body,
                                     item.request_line, item.request_bytes, item.request_started_at);
    }
}

}  // namespace nebula::server
