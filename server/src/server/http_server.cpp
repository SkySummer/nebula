#include "nebula/server/http_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nebula/common/logger.hpp"
#include "nebula/http/http_parser.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/net/address_discovery.hpp"
#include "nebula/server/signal_handler.hpp"

namespace nebula::server {

namespace {

constexpr std::size_t kReadChunkSize = 4096U;
constexpr int kEventWaitTimeoutMs = 100;
constexpr std::size_t kDefaultEventCapacity = 128U;
constexpr std::uint32_t kListenerEpollEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kWakeupEpollEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kConnectionReadEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kConnectionReadWriteEvents = EPOLLIN | EPOLLOUT | EPOLLET;

#ifdef MSG_NOSIGNAL
constexpr int kConnectionSendFlags = MSG_NOSIGNAL;
#else
constexpr int kConnectionSendFlags = 0;
#endif

bool is_would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}

void close_fd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

std::size_t max_pending_read_bytes(std::size_t max_header_bytes, std::size_t max_body_bytes) {
    if (max_header_bytes > (std::numeric_limits<std::size_t>::max() - max_body_bytes)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return max_header_bytes + max_body_bytes;
}

std::string format_allow_header(const std::vector<http::HttpMethod>& allowed_methods) {
    std::string allow;
    for (std::size_t idx = 0; idx < allowed_methods.size(); ++idx) {
        if (idx != 0U) {
            allow.append(", ");
        }
        allow.append(http::to_string(allowed_methods[idx]));
    }
    return allow;
}

std::string errno_message(int err) {
    const char* text = std::strerror(err);
    if (text == nullptr) {
        return "unknown";
    }
    return text;
}

void log_access_addresses(std::uint16_t listening_port) {
    const net::AccessAddressCollection access_addresses = net::collect_access_addresses(listening_port);
    for (const net::AccessAddress& address : access_addresses.addresses) {
        if (address.is_network) {
            common::Logger::instance()
                .info("server access network")
                .field("url", address.url)
                .field("port", listening_port)
                .field("interface", address.interface_name);
        } else {
            common::Logger::instance()
                .info("server access local")
                .field("url", address.url)
                .field("port", listening_port);
        }
    }
    if (access_addresses.interface_error != 0) {
        const int err = access_addresses.interface_error;
        common::Logger::instance()
            .warn("list network interfaces failed")
            .field("errno", err, errno_message(err))
            .field("fallback", "localhost_and_loopback_only");
    }
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

void report_destructor_cleanup_error(const char* action, const char* error) noexcept {
    std::fputs("server destructor cleanup failed: action=", stderr);
    std::fputs(action != nullptr ? action : "unknown", stderr);
    std::fputs(", error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputs(", decision=ignore", stderr);
    std::fputc('\n', stderr);
}

void report_log_emit_error(const char* event) noexcept {
    std::fputs("log emit failed: event=", stderr);
    std::fputs(event != nullptr ? event : "unknown", stderr);
    std::fputs(", error=logger_emit_failed", stderr);
    std::fputs(", decision=ignore", stderr);
    std::fputc('\n', stderr);
}

}  // namespace

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)),
      signal_handler_(config_.manage_signals ? std::make_unique<SignalHandler>() : nullptr),
      events_(kDefaultEventCapacity),
      thread_pool_(config_.worker_threads) {
    router_.add_route(http::HttpMethod::Get, "/", [](const http::HttpRequest&) {
        http::HttpResponse response;
        response.status = http::HttpStatus::TemporaryRedirect;
        response.headers.emplace("Location", "/healthz");
        return response;
    });

    router_.add_route(http::HttpMethod::Get, "/healthz", [](const http::HttpRequest&) {
        http::HttpResponse response;
        response.status = http::HttpStatus::OK;
        response.headers.emplace("Content-Type", "application/json");
        response.body = R"({"status":"ok"})";
        return response;
    });

    router_.add_route(http::HttpMethod::Post, "/echo", [](const http::HttpRequest& request) {
        http::HttpResponse response;
        response.status = http::HttpStatus::OK;
        response.headers.emplace("Content-Type", "application/json");
        response.body = request.body;
        return response;
    });
}

HttpServer::~HttpServer() noexcept {
    try {
        stop();
    } catch (const std::exception& e) {
        report_destructor_cleanup_error("stop", e.what());
    } catch (...) {
        report_destructor_cleanup_error("stop", "unknown");
    }

    try {
        thread_pool_.stop();
    } catch (const std::exception& e) {
        report_destructor_cleanup_error("thread_pool_stop", e.what());
    } catch (...) {
        report_destructor_cleanup_error("thread_pool_stop", "unknown");
    }

    try {
        shutdown_runtime();
    } catch (const std::exception& e) {
        report_destructor_cleanup_error("shutdown_runtime", e.what());
    } catch (...) {
        report_destructor_cleanup_error("shutdown_runtime", "unknown");
    }
}

bool HttpServer::add_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept {
    try {
        return router_.add_route(method, path, std::move(handler));
    } catch (const std::exception& e) {
        try {
            common::Logger::instance()
                .error("add route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", e.what());
        } catch (...) {
            report_log_emit_error("add_route_failed");
        }
        return false;
    } catch (...) {
        try {
            common::Logger::instance()
                .error("add route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", "unknown");
        } catch (...) {
            report_log_emit_error("add_route_failed");
        }
        return false;
    }
}

bool HttpServer::mod_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept {
    try {
        return router_.mod_route(method, path, std::move(handler));
    } catch (const std::exception& e) {
        try {
            common::Logger::instance()
                .error("mod route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", e.what());
        } catch (...) {
            report_log_emit_error("mod_route_failed");
        }
        return false;
    } catch (...) {
        try {
            common::Logger::instance()
                .error("mod route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", "unknown");
        } catch (...) {
            report_log_emit_error("mod_route_failed");
        }
        return false;
    }
}

bool HttpServer::del_route(http::HttpMethod method, const std::string& path) noexcept {
    try {
        return router_.del_route(method, path);
    } catch (const std::exception& e) {
        try {
            common::Logger::instance()
                .error("del route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", e.what());
        } catch (...) {
            report_log_emit_error("del_route_failed");
        }
        return false;
    } catch (...) {
        try {
            common::Logger::instance()
                .error("del route failed")
                .field("method", http::to_string(method))
                .field("path", path)
                .field("error", "unknown");
        } catch (...) {
            report_log_emit_error("del_route_failed");
        }
        return false;
    }
}

std::string_view HttpServer::to_string(LifecycleState state) {
    switch (state) {
        case LifecycleState::Idle:
            return "idle";
        case LifecycleState::Starting:
            return "starting";
        case LifecycleState::Running:
            return "running";
        case LifecycleState::Stopping:
            return "stopping";
    }
    return "unknown";
}

std::string_view HttpServer::to_string(RunResult result) {
    switch (result) {
        case RunResult::StartRejected:
            return "start_rejected";
        case RunResult::StartCanceled:
            return "start_canceled";
        case RunResult::StartFailed:
            return "start_failed";
        case RunResult::GracefulCompleted:
            return "graceful_completed";
        case RunResult::ForcedByTimeout:
            return "forced_by_timeout";
        case RunResult::FatalError:
            return "fatal_error";
        case RunResult::CleanupFailed:
            return "cleanup_failed";
    }
    return "unknown";
}

bool HttpServer::is_successful_run_result(RunResult result) {
    return result == RunResult::StartCanceled || result == RunResult::GracefulCompleted ||
           result == RunResult::ForcedByTimeout;
}

HttpServer::RunResult HttpServer::start() noexcept {
    LifecycleState expected_state = LifecycleState::Idle;
    bool runtime_initialized = false;
    SignalHandler* active_signal_handler = nullptr;
    response_submission_enabled_.store(false);
    auto cleanup_start_context = [&]() noexcept -> bool {
        if (active_signal_handler != nullptr) {
            active_signal_handler->stop();
            active_signal_handler = nullptr;
        }

        bool shutdown_ok = true;
        if (runtime_initialized) {
            lifecycle_state_.store(LifecycleState::Stopping);
            response_submission_enabled_.store(false);
            try {
                shutdown_runtime();
            } catch (...) {
                shutdown_ok = false;
            }
            runtime_initialized = false;
        }
        lifecycle_state_.store(LifecycleState::Idle);
        return shutdown_ok;
    };

    try {
        if (!lifecycle_state_.compare_exchange_strong(expected_state, LifecycleState::Starting)) {
            common::Logger::instance().warn("server start rejected").field("state", to_string(expected_state));
            return RunResult::StartRejected;
        }

        if (prestart_stop_requested_.exchange(false)) {
            cleanup_start_context();
            common::Logger::instance()
                .info("server start canceled")
                .field("reason", "stop_before_runtime_init")
                .field("state", to_string(LifecycleState::Starting))
                .field("next_state", to_string(LifecycleState::Idle));
            return RunResult::StartCanceled;
        }

        if (signal_handler_ != nullptr) {
            active_signal_handler = signal_handler_.get();
            active_signal_handler->start(*this);
        }

        runtime_initialized = true;
        if (!init_runtime()) {
            cleanup_start_context();
            return RunResult::StartFailed;
        }

        expected_state = LifecycleState::Starting;
        if (!lifecycle_state_.compare_exchange_strong(expected_state, LifecycleState::Running)) {
            cleanup_start_context();
            common::Logger::instance()
                .info("server start canceled")
                .field("reason", "stop_during_runtime_init")
                .field("state", to_string(expected_state))
                .field("next_state", to_string(LifecycleState::Idle));
            return RunResult::StartCanceled;
        }

        const std::uint16_t listening_port = listening_port_.load();
        common::Logger::instance()
            .info("server started")
            .field("port", listening_port)
            .field("backlog", config_.backlog)
            .field("max_connections", config_.max_connections)
            .field("timeout_ms", config_.read_timeout.count());

        log_access_addresses(listening_port);

        const RunResult event_loop_result = run_event_loop();
        const bool shutdown_ok = cleanup_start_context();

        if (event_loop_result == RunResult::FatalError) {
            common::Logger::instance().error("server stopped unexpectedly").field("error", "event_loop_fatal_error");
            return RunResult::FatalError;
        }
        if (!shutdown_ok) {
            common::Logger::instance().error("server stop cleanup failed").field("error", "shutdown_runtime_exception");
            return RunResult::CleanupFailed;
        }

        common::Logger::instance().info("server stopped").field("result", to_string(event_loop_result));
        return event_loop_result;
    } catch (const std::exception& e) {
        cleanup_start_context();
        try {
            common::Logger::instance()
                .error("server start exception")
                .field("state", to_string(lifecycle_state_.load()))
                .field("error", e.what());
        } catch (...) {
            report_log_emit_error("server_start_exception");
        }
        return RunResult::FatalError;
    } catch (...) {
        cleanup_start_context();
        try {
            common::Logger::instance()
                .error("server start exception")
                .field("state", to_string(lifecycle_state_.load()))
                .field("error", "unknown");
        } catch (...) {
            report_log_emit_error("server_start_exception");
        }
        return RunResult::FatalError;
    }
}

void HttpServer::stop() noexcept {
    try {
        LifecycleState state = lifecycle_state_.load();
        while (true) {
            if (state == LifecycleState::Starting || state == LifecycleState::Running) {
                if (lifecycle_state_.compare_exchange_weak(state, LifecycleState::Stopping)) {
                    prestart_stop_requested_.store(false);
                    common::Logger::instance()
                        .info("server stop requested")
                        .field("decision", "enter_stopping")
                        .field("state", to_string(state))
                        .field("next_state", to_string(LifecycleState::Stopping));
                    break;
                }
                continue;
            }

            if (state == LifecycleState::Idle) {
                prestart_stop_requested_.store(true);
                const LifecycleState observed = lifecycle_state_.load();
                if (observed != LifecycleState::Idle) {
                    state = observed;
                    continue;
                }
                common::Logger::instance()
                    .info("server stop requested")
                    .field("decision", "cancel_next_start")
                    .field("state", to_string(state))
                    .field("next_state", to_string(LifecycleState::Idle));
                break;
            }

            common::Logger::instance().debug("server stop ignored").field("state", to_string(state));
            break;
        }
        notify_wakeup();
    } catch (const std::exception& e) {
        try {
            common::Logger::instance()
                .error("server stop exception")
                .field("state", to_string(lifecycle_state_.load()))
                .field("error", e.what());
        } catch (...) {
            report_log_emit_error("server_stop_exception");
        }
    } catch (...) {
        try {
            common::Logger::instance()
                .error("server stop exception")
                .field("state", to_string(lifecycle_state_.load()))
                .field("error", "unknown");
        } catch (...) {
            report_log_emit_error("server_stop_exception");
        }
    }
}

bool HttpServer::is_running() const {
    return lifecycle_state_.load() == LifecycleState::Running;
}

std::uint16_t HttpServer::listening_port() const {
    return listening_port_.load();
}

bool HttpServer::init_runtime() {
    if (!listener_.open(config_.port, config_.backlog)) {
        const int err = errno;
        try {
            common::Logger::instance()
                .error("open listener failed")
                .field("port", config_.port)
                .field("errno", err, errno_message(err));
        } catch (...) {
            report_log_emit_error("open_listener_failed");
        }
        return false;
    }

    if (!epoll_.open()) {
        const int err = errno;
        listener_.close();
        try {
            common::Logger::instance()
                .error("open epoll failed")
                .field("port", config_.port)
                .field("errno", err, errno_message(err));
        } catch (...) {
            report_log_emit_error("open_epoll_failed");
        }
        return false;
    }

    const int wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        const int err = errno;
        epoll_.close();
        listener_.close();
        try {
            common::Logger::instance()
                .error("create wakeup fd failed")
                .field("port", config_.port)
                .field("errno", err, errno_message(err));
        } catch (...) {
            report_log_emit_error("create_wakeup_fd_failed");
        }
        return false;
    }

    if (!epoll_.add(listener_.fd(), kListenerEpollEvents)) {
        const int err = errno;
        const int listener_fd = listener_.fd();
        close_fd(wakeup_fd);
        epoll_.close();
        listener_.close();
        try {
            common::Logger::instance()
                .error("epoll add listener failed")
                .field("fd", listener_fd)
                .field("errno", err, errno_message(err));
        } catch (...) {
            report_log_emit_error("epoll_add_listener_failed");
        }
        return false;
    }

    if (!epoll_.add(wakeup_fd, kWakeupEpollEvents)) {
        const int err = errno;
        close_fd(wakeup_fd);
        epoll_.close();
        listener_.close();
        try {
            common::Logger::instance()
                .error("epoll add wakeup failed")
                .field("fd", wakeup_fd)
                .field("errno", err, errno_message(err));
        } catch (...) {
            report_log_emit_error("epoll_add_wakeup_failed");
        }
        return false;
    }

    {
        std::lock_guard lock(wakeup_mutex_);
        wakeup_fd_ = wakeup_fd;
    }

    listening_port_.store(listener_.port());
    return true;
}

void HttpServer::shutdown_runtime() {
    response_submission_enabled_.store(false);

    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto& [fd, connection] : connections_) {
        fds.push_back(fd);
    }
    for (int fd : fds) {
        close_connection(fd, "shutdown");
    }
    connections_.clear();

    std::size_t dropped_pending_count = 0;
    {
        std::lock_guard lock(pending_mutex_);
        dropped_pending_count = pending_responses_.size();
        std::vector<PendingResponse>().swap(pending_responses_);
    }
    if (dropped_pending_count > 0) {
        common::Logger::instance().info("pending responses cleared").field("count", dropped_pending_count);
    }

    {
        std::lock_guard lock(wakeup_mutex_);
        if (wakeup_fd_ >= 0) {
            close_fd(wakeup_fd_);
            wakeup_fd_ = -1;
        }
    }

    epoll_.close();
    listener_.close();
    listening_port_.store(0);
}

HttpServer::RunResult HttpServer::run_event_loop() {
    response_submission_enabled_.store(true);

    bool shutdown_started = false;
    std::chrono::steady_clock::time_point shutdown_deadline{};
    bool listener_closed_for_shutdown = false;
    RunResult stop_result = RunResult::GracefulCompleted;

    while (true) {
        const LifecycleState state = lifecycle_state_.load();
        if (state != LifecycleState::Running && state != LifecycleState::Stopping) {
            break;
        }

        if (state == LifecycleState::Stopping) {
            begin_graceful_shutdown(shutdown_started, shutdown_deadline, listener_closed_for_shutdown);
        }

        const int ready_count = epoll_.wait(events_, kEventWaitTimeoutMs);
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            common::Logger::instance()
                .error("epoll wait failed")
                .field("fd", epoll_.fd())
                .field("errno", err, errno_message(err))
                .field("decision", "stop_loop");
            response_submission_enabled_.store(false);
            return RunResult::FatalError;
        }

        process_ready_events(ready_count);

        sweep_idle_connections();

        if (lifecycle_state_.load() != LifecycleState::Stopping) {
            continue;
        }

        std::size_t pending_count = 0;
        if (!has_inflight_work(pending_count)) {
            common::Logger::instance()
                .info("graceful shutdown completed")
                .field("count", connections_.size())
                .field("pending_count", pending_count)
                .field("decision", "stop_loop");
            break;
        }

        if (shutdown_started && std::chrono::steady_clock::now() >= shutdown_deadline) {
            pending_count = pending_response_count();
            common::Logger::instance()
                .warn("graceful shutdown timeout")
                .field("timeout_ms", config_.graceful_shutdown_timeout.count())
                .field("count", connections_.size())
                .field("pending_count", pending_count)
                .field("decision", "force_close");
            stop_result = RunResult::ForcedByTimeout;
            break;
        }
    }

    response_submission_enabled_.store(false);
    return stop_result;
}

void HttpServer::begin_graceful_shutdown(bool& shutdown_started,
                                         std::chrono::steady_clock::time_point& shutdown_deadline,
                                         bool& listener_closed_for_shutdown) {
    close_listener_for_shutdown(listener_closed_for_shutdown);
    const std::size_t closed_count = close_idle_connections_for_shutdown();
    if (shutdown_started) {
        return;
    }

    shutdown_started = true;
    shutdown_deadline = std::chrono::steady_clock::now() + config_.graceful_shutdown_timeout;
    common::Logger::instance()
        .info("graceful shutdown started")
        .field("timeout_ms", config_.graceful_shutdown_timeout.count())
        .field("count", connections_.size())
        .field("pending_count", pending_response_count())
        .field("closed_count", closed_count)
        .field("next_state", "draining");
}

void HttpServer::close_listener_for_shutdown(bool& listener_closed_for_shutdown) {
    if (listener_closed_for_shutdown) {
        return;
    }
    listener_closed_for_shutdown = true;

    const int listener_fd = listener_.fd();
    if (listener_fd < 0) {
        return;
    }

    if (!epoll_.del(listener_fd)) {
        const int err = errno;
        common::Logger::instance()
            .warn("epoll del listener failed")
            .field("fd", listener_fd)
            .field("errno", err, errno_message(err))
            .field("decision", "continue_shutdown");
    }

    listener_.close();
    listening_port_.store(0);
    common::Logger::instance()
        .info("listener closed for shutdown")
        .field("fd", listener_fd)
        .field("next_state", "draining");
}

std::size_t HttpServer::close_idle_connections_for_shutdown() {
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

std::size_t HttpServer::pending_response_count() {
    std::lock_guard lock(pending_mutex_);
    return pending_responses_.size();
}

bool HttpServer::has_inflight_work(std::size_t& pending_count) {
    pending_count = pending_response_count();
    if (pending_count > 0) {
        return true;
    }
    return std::ranges::any_of(connections_, [](const auto& entry) {
        const Connection& connection = entry.second;
        return connection.processing || !connection.write_buffer.empty();
    });
}

void HttpServer::process_ready_events(int ready_count) {
    for (int idx = 0; idx < ready_count; ++idx) {
        const epoll_event& event = events_[static_cast<std::size_t>(idx)];
        const int fd = event.data.fd;
        const int wakeup_fd = load_wakeup_fd();
        const int listener_fd = listener_.fd();

        if (listener_fd >= 0 && fd == listener_fd) {
            accept_new_connections();
            continue;
        }

        if (wakeup_fd >= 0 && fd == wakeup_fd) {
            drain_wakeup(wakeup_fd);
            drain_pending_responses();
            continue;
        }

        handle_client_event(fd, event.events);
    }
}

void HttpServer::accept_new_connections() {
    while (lifecycle_state_.load() == LifecycleState::Running) {
        const net::AcceptedSocket accepted = listener_.accept_one();
        if (accepted.fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!is_would_block(errno)) {
                const int err = errno;
                common::Logger::instance()
                    .warn("accept failed")
                    .field("fd", listener_.fd())
                    .field("errno", err, errno_message(err))
                    .field("decision", "keep_running");
            }
            return;
        }

        if (connections_.size() >= config_.max_connections) {
            common::Logger::instance()
                .warn("too many connections")
                .field("count", connections_.size())
                .field("max_connections", config_.max_connections)
                .field("peer", accepted.peer)
                .field("result", "rejected");
            close_fd(accepted.fd);
            continue;
        }

        if (!epoll_.add(accepted.fd, kConnectionReadEvents)) {
            const int err = errno;
            common::Logger::instance()
                .error("epoll add connection failed")
                .field("fd", accepted.fd)
                .field("errno", err, errno_message(err))
                .field("next_state", "closing");
            close_fd(accepted.fd);
            continue;
        }

        Connection connection;
        connection.fd = accepted.fd;
        connection.token = next_connection_token_++;
        connection.peer = accepted.peer;
        connection.last_active = std::chrono::steady_clock::now();
        connections_.emplace(accepted.fd, std::move(connection));
        common::Logger::instance().debug("connection accepted").field("fd", accepted.fd).field("peer", accepted.peer);
    }
}

void HttpServer::handle_client_event(int fd, std::uint32_t events) {
    if ((events & EPOLLERR) != 0U || (events & EPOLLHUP) != 0U) {
        close_connection(fd, "epoll_error");
        return;
    }

    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
        common::Logger::instance().debug("unknown connection event").field("fd", fd).field("events", events);
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

void HttpServer::handle_readable(Connection& connection) {
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
                        .warn("pending request bytes exceeded")
                        .field("fd", connection.fd)
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

        if (is_would_block(errno)) {
            break;
        }

        const int err = errno;
        common::Logger::instance()
            .warn("read failed")
            .field("fd", connection.fd)
            .field("errno", err, errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "read_failed");
        return;
    }

    if (!connection.processing && connection.write_buffer.empty()) {
        parse_next_request(connection);
    }
}

void HttpServer::handle_writable(Connection& connection) {
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

        if (sent_n < 0 && is_would_block(errno)) {
            return;
        }

        const int err = errno;
        common::Logger::instance()
            .warn("write failed")
            .field("fd", connection.fd)
            .field("errno", err, errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "write_failed");
        return;
    }

    if (connection.close_after_write) {
        close_connection(connection.fd, "response_completed");
        return;
    }

    if (lifecycle_state_.load() == LifecycleState::Stopping) {
        close_connection(connection.fd, "graceful_shutdown");
        return;
    }

    if (!epoll_.mod(connection.fd, kConnectionReadEvents)) {
        const int err = errno;
        common::Logger::instance()
            .warn("epoll mod read failed")
            .field("fd", connection.fd)
            .field("errno", err, errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "epoll_mod_failed");
        return;
    }

    parse_next_request(connection);
}

void HttpServer::schedule_request(Connection& connection, http::HttpRequest request, std::size_t request_bytes) {
    if (lifecycle_state_.load() != LifecycleState::Running) {
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
        thread_pool_.submit([this, fd, connection_token, close_after_write, suppress_body, request_line, request_bytes,
                             request_started_at, request = std::move(request)]() mutable {
            try {
                http::HttpResponse response = dispatch_request(request);
                submit_response(fd, connection_token, std::move(response), close_after_write, suppress_body,
                                request_line, request_bytes, request_started_at);
            } catch (const std::exception& e) {
                common::Logger::instance()
                    .error("request handler exception")
                    .field("fd", fd)
                    .field("token", connection_token)
                    .field("error", e.what())
                    .field("next_state", "closing");
                submit_error_response(fd, connection_token, http::HttpStatus::InternalServerError, {}, true,
                                      suppress_body, request_line, request_bytes, request_started_at);
            } catch (...) {
                common::Logger::instance()
                    .error("request handler exception")
                    .field("fd", fd)
                    .field("token", connection_token)
                    .field("error", "unknown")
                    .field("next_state", "closing");
                submit_error_response(fd, connection_token, http::HttpStatus::InternalServerError, {}, true,
                                      suppress_body, request_line, request_bytes, request_started_at);
            }
        });
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("thread pool submit failed")
            .field("fd", fd)
            .field("token", connection_token)
            .field("error", e.what())
            .field("fallback", "sync")
            .field("next_state", "closing");
        submit_error_response(fd, connection_token, http::HttpStatus::InternalServerError, {}, true, suppress_body,
                              request_line, request_bytes, request_started_at);
    }
}

void HttpServer::parse_next_request(Connection& connection) {
    if (connection.processing || !connection.write_buffer.empty()) {
        return;
    }

    if (lifecycle_state_.load() == LifecycleState::Stopping) {
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
            submit_error_response(connection.fd, connection.token, parsed.http_status, parsed.error, true,
                                  suppress_body, request_line, request_bytes, request_started_at);
            return;
        }
        case http::ParseStatus::Complete:
            break;
    }

    connection.read_buffer.erase(0, parsed.consumed_bytes);
    schedule_request(connection, std::move(parsed.request), parsed.consumed_bytes);
}

void HttpServer::submit_response(int fd, std::uint64_t connection_token, http::HttpResponse response,
                                 bool close_after_write, bool suppress_body, std::string request_line,
                                 std::size_t request_bytes, std::chrono::steady_clock::time_point request_started_at) {
    const LifecycleState state = lifecycle_state_.load();
    const bool can_submit_now =
        state == LifecycleState::Running || (state == LifecycleState::Stopping && response_submission_enabled_.load());
    if (!can_submit_now) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", fd)
            .field("token", connection_token)
            .field("state", to_string(state));
        return;
    }

    {
        std::lock_guard lock(pending_mutex_);
        const LifecycleState state_after_lock = lifecycle_state_.load();
        const bool can_submit_after_lock =
            state_after_lock == LifecycleState::Running ||
            (state_after_lock == LifecycleState::Stopping && response_submission_enabled_.load());
        if (!can_submit_after_lock) {
            common::Logger::instance()
                .debug("response dropped")
                .field("fd", fd)
                .field("token", connection_token)
                .field("state", to_string(state_after_lock));
            return;
        }
        pending_responses_.push_back(PendingResponse{
            .fd = fd,
            .token = connection_token,
            .response = std::move(response),
            .close_after_write = close_after_write,
            .suppress_body = suppress_body,
            .request_line = std::move(request_line),
            .request_bytes = request_bytes,
            .request_started_at = request_started_at,
        });
    }

    notify_wakeup();
}

void HttpServer::submit_error_response(int fd, std::uint64_t connection_token, http::HttpStatus status,
                                       std::string body, bool close_after_write, bool suppress_body,
                                       std::string request_line, std::size_t request_bytes,
                                       std::chrono::steady_clock::time_point request_started_at) {
    submit_response(fd, connection_token, http::make_error_response(status, std::move(body)), close_after_write,
                    suppress_body, std::move(request_line), request_bytes, request_started_at);
}

void HttpServer::apply_response_to_connection(Connection& connection, const http::HttpResponse& response,
                                              bool close_after_write, bool suppress_body, std::string_view request_line,
                                              std::size_t request_bytes,
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
            .warn("epoll mod write failed")
            .field("fd", connection.fd)
            .field("errno", err, errno_message(err))
            .field("next_state", "closing");
        close_connection(connection.fd, "epoll_mod_failed");
    }
}

void HttpServer::drain_wakeup(int wakeup_fd) {
    std::uint64_t count = 0;
    while (true) {
        const ssize_t read_n = ::read(wakeup_fd, &count, sizeof(count));
        if (read_n > 0) {
            continue;
        }
        if (read_n < 0 && errno == EINTR) {
            continue;
        }
        if (read_n < 0 && is_would_block(errno)) {
            return;
        }
        if (read_n < 0) {
            const int err = errno;
            common::Logger::instance()
                .warn("drain wakeup failed")
                .field("fd", wakeup_fd)
                .field("errno", err, errno_message(err))
                .field("decision", "keep_running");
        }
        return;
    }
}

void HttpServer::drain_pending_responses() {
    std::vector<PendingResponse> local_responses;
    {
        std::lock_guard lock(pending_mutex_);
        if (pending_responses_.empty()) {
            return;
        }
        local_responses.swap(pending_responses_);
    }

    for (PendingResponse& item : local_responses) {
        const auto it = connections_.find(item.fd);
        if (it == connections_.end()) {
            continue;
        }
        if (it->second.token != item.token) {
            common::Logger::instance()
                .debug("stale pending response ignored")
                .field("fd", item.fd)
                .field("pending_token", item.token)
                .field("active_token", it->second.token);
            continue;
        }
        apply_response_to_connection(it->second, item.response, item.close_after_write, item.suppress_body,
                                     item.request_line, item.request_bytes, item.request_started_at);
    }
}

void HttpServer::notify_wakeup() {
    std::lock_guard lock(wakeup_mutex_);
    if (wakeup_fd_ < 0) {
        return;
    }

    const std::uint64_t one = 1;
    const ssize_t write_n = ::write(wakeup_fd_, &one, sizeof(one));
    if (write_n < 0 && !is_would_block(errno)) {
        const int err = errno;
        common::Logger::instance()
            .warn("notify wakeup failed")
            .field("fd", wakeup_fd_)
            .field("errno", err, errno_message(err))
            .field("decision", "keep_running");
    }
}

int HttpServer::load_wakeup_fd() const {
    std::lock_guard lock(wakeup_mutex_);
    return wakeup_fd_;
}

void HttpServer::close_connection(int fd, std::string_view close_reason) {
    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    Connection connection = std::move(it->second);
    connections_.erase(it);
    if (!epoll_.del(fd)) {
        const int err = errno;
        common::Logger::instance()
            .warn("epoll del connection failed")
            .field("fd", fd)
            .field("errno", err, errno_message(err))
            .field("decision", "continue_close");
    }
    close_fd(fd);

    common::Logger::instance()
        .debug("connection closed")
        .field("fd", fd)
        .field("peer", connection.peer)
        .field("close_reason", close_reason);
}

void HttpServer::sweep_idle_connections() {
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
            .info("idle connection timeout")
            .field("fd", fd)
            .field("peer", peer)
            .field("timeout_ms", config_.read_timeout.count());
        close_connection(fd, "idle_timeout");
    }
}

http::HttpResponse HttpServer::dispatch_request(const http::HttpRequest& request) {
    const http::RouteDispatchResult routed = router_.dispatch(request);
    if (routed.status == http::RouteStatus::Matched) {
        return routed.response;
    }
    if (routed.status == http::RouteStatus::MethodNotAllowed) {
        http::HttpResponse response = http::make_error_response(http::HttpStatus::MethodNotAllowed);
        const std::string allow = format_allow_header(routed.allowed_methods);
        if (!allow.empty()) {
            response.headers["Allow"] = allow;
        }
        return response;
    }
    return http::make_error_response(http::HttpStatus::NotFound);
}

}  // namespace nebula::server
