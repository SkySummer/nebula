#include "nebula/server/http_sub_reactor.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/http/http_parser.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/net/eventfd_utils.hpp"
#include "nebula/server/reactor_constants.hpp"

namespace nebula::server {

namespace {

constexpr std::size_t kReadChunkSize = 4096U;

#ifdef MSG_NOSIGNAL
constexpr int kConnectionSendFlags = MSG_NOSIGNAL;
#else
constexpr int kConnectionSendFlags = 0;
#endif

std::size_t max_pending_read_bytes(std::size_t max_header_bytes, std::size_t max_body_bytes) {
    if (max_header_bytes > (std::numeric_limits<std::size_t>::max() - max_body_bytes)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return max_header_bytes + max_body_bytes;
}

std::string extract_request_line(std::string_view read_buffer) {
    if (read_buffer.empty()) {
        return {};
    }

    std::size_t line_end = read_buffer.find("\r\n");
    if (line_end == std::string_view::npos) {
        line_end = read_buffer.find('\n');
    }
    if (line_end == std::string_view::npos) {
        line_end = read_buffer.size();
    }
    return std::string(read_buffer.substr(0, line_end));
}

bool is_head_request_line(std::string_view request_line) {
    return request_line.starts_with("HEAD ");
}

std::string quote_request_line_for_log(std::string_view request_line) {
    std::string quoted;
    quoted.reserve(request_line.size() + 2U);
    quoted.push_back('"');
    for (const unsigned char ch : request_line) {
        if (ch == '\\' || ch == '"') {
            quoted.push_back('\\');
            quoted.push_back(static_cast<char>(ch));
            continue;
        }

        if (ch < 0x20U || ch == 0x7FU) {
            quoted.push_back('?');
            continue;
        }
        quoted.push_back(static_cast<char>(ch));
    }
    quoted.push_back('"');
    return quoted;
}

}  // namespace

void HttpSubReactor::process_ready_events(int ready_count) {
    for (int idx = 0; idx < ready_count; ++idx) {
        const epoll_event& event = events_[static_cast<std::size_t>(idx)];
        const int fd = event.data.fd;
        const int wakeup_fd = load_wakeup_fd();

        if (wakeup_fd >= 0 && fd == wakeup_fd) {
            drain_wakeup(wakeup_fd);
            drain_pending_accepts();
            drain_pending_responses();
            continue;
        }

        handle_client_event(fd, event.events);
    }
}

void HttpSubReactor::handle_client_event(int fd, std::uint32_t events) {
    if ((events & EPOLLERR) != 0U || (events & EPOLLHUP) != 0U) {
        close_connection(fd, "epoll_error");
        return;
    }

    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
        common::Logger::instance()
            .debug("sub reactor unknown connection event")
            .field("fd", fd)
            .field("reactor_id", id_)
            .field("events", events);
        return;
    }

    Connection& connection = it->second;
    if ((events & EPOLLIN) != 0U) {
        handle_readable(connection);
    }

    if ((events & EPOLLOUT) != 0U) {
        const auto still_exists = connections_.find(fd);
        if (still_exists != connections_.end()) {
            handle_writable(still_exists->second);
        }
    }
}

void HttpSubReactor::handle_readable(Connection& connection) {
    connection.last_active = std::chrono::steady_clock::now();
    const std::size_t pending_limit = max_pending_read_bytes(config_.max_header_bytes, config_.max_body_bytes);

    std::vector<char> buffer(kReadChunkSize);
    while (true) {
        const ssize_t read_n = ::recv(connection.fd, buffer.data(), buffer.size(), 0);
        if (read_n > 0) {
            connection.last_active = std::chrono::steady_clock::now();
            if (!connection.close_after_write) {
                connection.read_buffer.append(buffer.data(), static_cast<std::size_t>(read_n));
                if (connection.read_buffer.size() > pending_limit) {
                    common::Logger::instance()
                        .warn("sub reactor pending request bytes exceeded")
                        .field("fd", connection.fd)
                        .field("reactor_id", id_)
                        .field("pending_bytes", connection.read_buffer.size())
                        .field("max_pending_bytes", pending_limit)
                        .field("next_state", "closing");
                    close_connection(connection.fd, "pending_bytes_exceeded");
                    return;
                }
            }

            if (!connection.processing && connection.write_buffer.empty()) {
                parse_next_request(connection);
            }
            continue;
        }

        if (read_n == 0) {
            close_connection(connection.fd, "peer_closed");
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (common::is_would_block(errno)) {
            break;
        }

        const int err = errno;
        common::Logger::instance()
            .warn("sub reactor read failed")
            .field("fd", connection.fd)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "read_failed");
        return;
    }

    if (!connection.processing && connection.write_buffer.empty()) {
        parse_next_request(connection);
    }
}

void HttpSubReactor::handle_writable(Connection& connection) {
    while (!connection.write_buffer.empty()) {
        const ssize_t sent_n =
            ::send(connection.fd, connection.write_buffer.data(), connection.write_buffer.size(), kConnectionSendFlags);
        if (sent_n > 0) {
            connection.write_buffer.erase(0, static_cast<std::size_t>(sent_n));
            connection.last_active = std::chrono::steady_clock::now();
            continue;
        }

        if (sent_n < 0 && errno == EINTR) {
            continue;
        }

        if (sent_n < 0 && common::is_would_block(errno)) {
            return;
        }

        const int err = errno;
        common::Logger::instance()
            .warn("sub reactor write failed")
            .field("fd", connection.fd)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "write_failed");
        return;
    }

    if (connection.close_after_write) {
        close_connection(connection.fd, "response_completed");
        return;
    }

    if (current_lifecycle_state() == LifecycleState::Stopping) {
        close_connection(connection.fd, "graceful_shutdown");
        return;
    }

    if (!epoll_.mod(connection.fd, kConnectionReadEvents)) {
        const int err = errno;
        common::Logger::instance()
            .warn("sub reactor epoll mod read failed")
            .field("fd", connection.fd)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "epoll_mod_failed");
        return;
    }

    parse_next_request(connection);
}

void HttpSubReactor::schedule_request(Connection& connection, http::HttpRequest request, std::size_t request_bytes) {
    if (current_lifecycle_state() != LifecycleState::Running) {
        connection.close_after_write = true;
        close_connection(connection.fd, "graceful_shutdown");
        return;
    }

    connection.processing = true;
    const bool close_after_write = !request.keep_alive;
    const bool suppress_body = request.method == http::HttpMethod::Head;
    connection.close_after_write |= close_after_write;
    const int fd = connection.fd;
    const std::uint64_t connection_token = connection.token;
    const std::string request_line = request.request_line;
    const auto request_started_at = std::chrono::steady_clock::now();

    try {
        if (!dispatch_request_) {
            throw std::runtime_error("request dispatch callback missing");
        }
        dispatch_request_(ReactorRequestTask{
            .reactor_id = id_,
            .fd = fd,
            .connection_token = connection_token,
            .request = std::move(request),
            .close_after_write = close_after_write,
            .suppress_body = suppress_body,
            .request_line = request_line,
            .request_bytes = request_bytes,
            .request_started_at = request_started_at,
        });
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("sub reactor thread pool submit failed")
            .field("fd", fd)
            .field("reactor_id", id_)
            .field("connection_token", connection_token)
            .field("error", e.what())
            .field("fallback", "sync")
            .field("next_state", "closing");
        try {
            enqueue_error_response(fd, connection_token, http::HttpStatus::InternalServerError, {}, true, suppress_body,
                                   request_line, request_bytes, request_started_at);
        } catch (const std::exception& enqueue_error) {
            close_with_response_enqueue_error("sub reactor enqueue error response failed", fd, connection_token,
                                              enqueue_error.what(), "response_enqueue_failed");
        } catch (...) {
            close_with_response_enqueue_error("sub reactor enqueue error response failed", fd, connection_token,
                                              "unknown", "response_enqueue_failed");
        }
    }
}

void HttpSubReactor::parse_next_request(Connection& connection) {
    if (connection.processing || !connection.write_buffer.empty()) {
        return;
    }

    if (current_lifecycle_state() == LifecycleState::Stopping) {
        connection.close_after_write = true;
        close_connection(connection.fd, "graceful_shutdown");
        return;
    }

    const std::string request_line = extract_request_line(connection.read_buffer);
    const std::size_t request_bytes = connection.read_buffer.size();
    const auto request_started_at = std::chrono::steady_clock::now();

    http::ParseResult parsed = http::parse_http_request(connection.read_buffer, config_.max_header_bytes,
                                                        config_.max_body_bytes, config_.max_request_target_bytes);
    switch (parsed.status) {
        case http::ParseStatus::NeedMore:
            return;
        case http::ParseStatus::Error: {
            connection.close_after_write = true;
            connection.processing = true;
            const bool suppress_body = is_head_request_line(request_line);
            try {
                enqueue_error_response(connection.fd, connection.token, parsed.http_status, parsed.error, true,
                                       suppress_body, request_line, request_bytes, request_started_at);
            } catch (const std::exception& e) {
                close_with_response_enqueue_error("sub reactor enqueue parse error response failed", connection.fd,
                                                  connection.token, e.what(), "response_enqueue_failed");
            } catch (...) {
                close_with_response_enqueue_error("sub reactor enqueue parse error response failed", connection.fd,
                                                  connection.token, "unknown", "response_enqueue_failed");
            }
            return;
        }
        case http::ParseStatus::Complete:
            break;
    }

    connection.read_buffer.erase(0, parsed.consumed_bytes);
    schedule_request(connection, std::move(parsed.request), parsed.consumed_bytes);
}

void HttpSubReactor::enqueue_error_response(int fd, std::uint64_t token, http::HttpStatus status, std::string body,
                                            bool close_after_write, bool suppress_body, std::string request_line,
                                            std::size_t request_bytes,
                                            std::chrono::steady_clock::time_point request_started_at) {
    const bool enqueued = enqueue_response(ReactorResponseTask{
        .reactor_id = id_,
        .fd = fd,
        .connection_token = token,
        .response = http::make_error_response(status, std::move(body)),
        .close_after_write = close_after_write,
        .suppress_body = suppress_body,
        .request_line = std::move(request_line),
        .request_bytes = request_bytes,
        .request_started_at = request_started_at,
    });
    if (!enqueued) {
        throw std::runtime_error("sub_reactor_not_running");
    }
}

void HttpSubReactor::close_with_response_enqueue_error(const char* event, int fd, std::uint64_t token,
                                                       const char* error, std::string_view close_reason) {
    common::Logger::instance()
        .error(event != nullptr ? event : "sub reactor enqueue response failed")
        .field("fd", fd)
        .field("reactor_id", id_)
        .field("connection_token", token)
        .field("error", error != nullptr ? error : "unknown")
        .field("next_state", "closing");
    close_connection(fd, close_reason);
}

void HttpSubReactor::apply_response_to_connection(Connection& connection, const http::HttpResponse& response,
                                                  bool close_after_write, bool suppress_body,
                                                  std::string_view request_line, std::size_t request_bytes,
                                                  std::chrono::steady_clock::time_point request_started_at) {
    connection.processing = false;
    const std::string serialized_response = http::serialize_http_response(response, !close_after_write, suppress_body);
    const auto latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - request_started_at)
            .count();
    const std::string quoted_request = quote_request_line_for_log(request_line);

    common::Logger::instance()
        .info("request completed")
        .field("fd", connection.fd)
        .field("peer", connection.peer)
        .field("reactor_id", id_)
        .field("request", quoted_request)
        .field("status", response.status_code(), response.status_text())
        .field("request_bytes", request_bytes)
        .field("response_bytes", serialized_response.size())
        .field("latency_ms", latency_ms);

    connection.write_buffer.append(serialized_response);
    connection.close_after_write |= close_after_write;

    if (!epoll_.mod(connection.fd, kConnectionReadWriteEvents)) {
        const int err = errno;
        common::Logger::instance()
            .warn("sub reactor epoll mod write failed")
            .field("fd", connection.fd)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "epoll_mod_failed");
    }
}

void HttpSubReactor::drain_wakeup(int wakeup_fd) {
    const int err = net::drain_eventfd(wakeup_fd);
    if (err != 0) {
        common::Logger::instance()
            .warn("sub reactor drain wakeup failed")
            .field("fd", wakeup_fd)
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_running");
    }
}

}  // namespace nebula::server
