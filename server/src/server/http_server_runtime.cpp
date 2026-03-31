#include "nebula/server/http_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/http/router.hpp"
#include "nebula/net/address_discovery.hpp"
#include "nebula/server/http_main_reactor.hpp"
#include "nebula/server/http_request_dispatcher.hpp"
#include "nebula/server/http_sub_reactor_pool.hpp"
#include "nebula/server/reactor_constants.hpp"
#include "nebula/server/signal_handler.hpp"

namespace nebula::server {

namespace {

ServerConfig normalize_server_config(ServerConfig config) {
    normalize_server_thread_counts(config);
    return config;
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
            .field("errno", err, common::errno_message(err))
            .field("fallback", "localhost_and_loopback_only");
    }
}

void report_destructor_cleanup_error(const char* action, const char* error) noexcept {
    std::fputs("runtime destructor cleanup failed: action=", stderr);
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

void log_runtime_exception(const char* event, LifecycleState state, const char* error) noexcept {
    try {
        common::Logger::instance()
            .error(event != nullptr ? event : "runtime exception")
            .field("state", to_string(state))
            .field("error", error != nullptr ? error : "unknown");
    } catch (...) {
        report_log_emit_error(event);
    }
}

}  // namespace

HttpServerRuntime::HttpServerRuntime(ServerConfig config, std::shared_ptr<http::Router> router)
    : config_(normalize_server_config(std::move(config))),
      signal_handler_(config_.manage_signals ? std::make_unique<SignalHandler>() : nullptr),
      main_reactor_(std::make_unique<HttpMainReactor>()),
      router_(std::move(router)),
      thread_pool_(config_.worker_thread_count) {
    if (router_ == nullptr) {
        router_ = std::make_shared<http::Router>();
    }

    request_dispatcher_ = std::make_unique<HttpRequestDispatcher>(
        router_, thread_pool_, [this](ReactorResponseTask task) { submit_response(std::move(task)); });

    sub_reactor_pool_ = std::make_unique<HttpSubReactorPool>(
        config_, [this](ReactorRequestTask task) { dispatch_sub_request(std::move(task)); },
        [this]() { return lifecycle_controller_.state(); }, [this]() { return force_close_requested_.load(); },
        [this](std::size_t reactor_id) { on_sub_reactor_fatal(reactor_id); });
}

HttpServerRuntime::~HttpServerRuntime() noexcept {
    auto run_cleanup = [](const char* action, const auto& cleanup) noexcept {
        try {
            cleanup();
        } catch (const std::exception& e) {
            report_destructor_cleanup_error(action, e.what());
        } catch (...) {
            report_destructor_cleanup_error(action, "unknown");
        }
    };

    run_cleanup("request_stop", [this]() { request_stop(); });
    run_cleanup("thread_pool_stop", [this]() { thread_pool_.stop(); });
    run_cleanup("shutdown_runtime", [this]() { shutdown_runtime(); });
}

RunResult HttpServerRuntime::run() noexcept {
    LifecycleState expected_state = LifecycleState::Idle;
    bool runtime_initialized = false;
    SignalHandler* active_signal_handler = nullptr;

    auto cleanup_start_context = [&]() noexcept -> bool {
        if (active_signal_handler != nullptr) {
            active_signal_handler->stop();
            active_signal_handler = nullptr;
        }

        bool shutdown_ok = true;
        if (runtime_initialized) {
            lifecycle_controller_.set_state(LifecycleState::Stopping);
            disable_response_submission();
            force_close_requested_.store(true);
            notify_main_wakeup();
            notify_sub_reactors();
            try {
                shutdown_runtime();
            } catch (...) {
                shutdown_ok = false;
            }
            runtime_initialized = false;
        }
        lifecycle_controller_.set_state(LifecycleState::Idle);
        return shutdown_ok;
    };

    try {
        if (!lifecycle_controller_.try_enter_starting(expected_state)) {
            common::Logger::instance().warn("runtime start rejected").field("state", to_string(expected_state));
            return RunResult::StartRejected;
        }

        response_submission_enabled_.store(false);
        force_close_requested_.store(false);
        sub_reactor_fatal_error_.store(false);

        if (lifecycle_controller_.consume_start_cancel_request()) {
            cleanup_start_context();
            common::Logger::instance()
                .info("runtime start canceled")
                .field("reason", "stop_before_runtime_init")
                .field("state", to_string(LifecycleState::Starting))
                .field("next_state", to_string(LifecycleState::Idle));
            return RunResult::StartCanceled;
        }

        if (signal_handler_ != nullptr) {
            active_signal_handler = signal_handler_.get();
            active_signal_handler->start([this]() { request_stop(); });
        }

        runtime_initialized = true;
        if (!init_runtime()) {
            cleanup_start_context();
            return RunResult::StartFailed;
        }

        expected_state = LifecycleState::Starting;
        if (!lifecycle_controller_.try_enter_running(expected_state)) {
            cleanup_start_context();
            common::Logger::instance()
                .info("runtime start canceled")
                .field("reason", "stop_during_runtime_init")
                .field("state", to_string(expected_state))
                .field("next_state", to_string(LifecycleState::Idle));
            return RunResult::StartCanceled;
        }

        const std::uint16_t active_port = listening_port_.load();
        common::Logger::instance()
            .info("runtime started")
            .field("port", active_port)
            .field("backlog", config_.backlog)
            .field("max_connections", config_.max_connections)
            .field("sub_reactor_count", config_.sub_reactor_count)
            .field("worker_thread_count", config_.worker_thread_count)
            .field("timeout_ms", config_.read_timeout.count());

        log_access_addresses(active_port);

        const RunResult event_loop_result = run_event_loop();
        const bool shutdown_ok = cleanup_start_context();

        if (event_loop_result == RunResult::FatalError) {
            common::Logger::instance().error("runtime stopped unexpectedly").field("error", "event_loop_fatal_error");
            return RunResult::FatalError;
        }
        if (!shutdown_ok) {
            common::Logger::instance().error("runtime cleanup failed").field("error", "shutdown_runtime_exception");
            return RunResult::CleanupFailed;
        }

        common::Logger::instance().info("runtime stopped").field("result", to_string(event_loop_result));
        return event_loop_result;
    } catch (const std::exception& e) {
        cleanup_start_context();
        log_runtime_exception("runtime start exception", lifecycle_controller_.state(), e.what());
        return RunResult::FatalError;
    } catch (...) {
        cleanup_start_context();
        log_runtime_exception("runtime start exception", lifecycle_controller_.state(), "unknown");
        return RunResult::FatalError;
    }
}

void HttpServerRuntime::request_stop() noexcept {
    try {
        const StopRequestTransition transition = lifecycle_controller_.request_stop();
        if (transition.decision != StopRequestDecision::Ignored) {
            common::Logger::instance()
                .info("runtime stop requested")
                .field("decision", to_string(transition.decision))
                .field("state", to_string(transition.state))
                .field("next_state", to_string(transition.next_state));
        } else {
            common::Logger::instance().debug("runtime stop ignored").field("state", to_string(transition.state));
        }

        notify_main_wakeup();
        notify_sub_reactors();
    } catch (const std::exception& e) {
        log_runtime_exception("runtime stop exception", lifecycle_controller_.state(), e.what());
    } catch (...) {
        log_runtime_exception("runtime stop exception", lifecycle_controller_.state(), "unknown");
    }
}

bool HttpServerRuntime::is_running() const noexcept {
    return lifecycle_controller_.state() == LifecycleState::Running;
}

std::uint16_t HttpServerRuntime::listening_port() const noexcept {
    return listening_port_.load();
}

bool HttpServerRuntime::init_runtime() {
    force_close_requested_.store(false);
    sub_reactor_fatal_error_.store(false);

    if (main_reactor_ == nullptr) {
        common::Logger::instance().error("runtime init failed").field("error", "main_reactor_missing");
        return false;
    }

    if (!main_reactor_->open(config_.port, config_.backlog)) {
        return false;
    }

    if (sub_reactor_pool_ == nullptr) {
        main_reactor_->close();
        common::Logger::instance().error("runtime init failed").field("error", "sub_reactor_pool_missing");
        return false;
    }

    if (!sub_reactor_pool_->start()) {
        main_reactor_->close();
        return false;
    }

    listening_port_.store(main_reactor_->listening_port());
    return true;
}

void HttpServerRuntime::shutdown_runtime() {
    disable_response_submission();
    force_close_requested_.store(true);

    if (sub_reactor_pool_ != nullptr) {
        sub_reactor_pool_->shutdown();
    }

    if (main_reactor_ != nullptr) {
        main_reactor_->close();
    }
    listening_port_.store(0);

    force_close_requested_.store(false);
    sub_reactor_fatal_error_.store(false);
}

RunResult HttpServerRuntime::run_event_loop() {
    response_submission_enabled_.store(true);
    force_close_requested_.store(false);

    ShutdownMachine shutdown{};

    while (true) {
        const LifecycleState state = lifecycle_controller_.state();
        if (state != LifecycleState::Running && state != LifecycleState::Stopping) {
            break;
        }

        const bool stopping = state == LifecycleState::Stopping;
        if (stopping && shutdown.state == ShutdownState::Serving) {
            enter_shutdown_draining(shutdown);
        }

        if (stopping && advance_shutdown_machine(shutdown)) {
            break;
        }

        if (main_reactor_ == nullptr) {
            common::Logger::instance().error("main reactor unavailable").field("error", "main_reactor_missing");
            disable_response_submission();
            return RunResult::FatalError;
        }

        if (!main_reactor_->wait_and_process_events([this]() { accept_new_connections(); },
                                                    compute_wait_timeout_ms(shutdown))) {
            disable_response_submission();
            return RunResult::FatalError;
        }

        if (sub_reactor_fatal_error_.load()) {
            common::Logger::instance()
                .error("sub reactor failed")
                .field("error", "sub_reactor_event_loop_fatal")
                .field("decision", "stop_loop");
            disable_response_submission();
            return RunResult::FatalError;
        }

        if (lifecycle_controller_.state() == LifecycleState::Stopping && advance_shutdown_machine(shutdown)) {
            break;
        }
    }

    disable_response_submission();
    return shutdown.result;
}

int HttpServerRuntime::compute_wait_timeout_ms(const ShutdownMachine& shutdown) {
    if (shutdown.state != ShutdownState::Draining) {
        return kEventWaitTimeoutMs;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= shutdown.deadline) {
        return 0;
    }

    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(shutdown.deadline - now).count();
    if (remaining_ms <= 0) {
        return 0;
    }
    if (remaining_ms >= static_cast<std::int64_t>(kEventWaitTimeoutMs)) {
        return kEventWaitTimeoutMs;
    }
    return static_cast<int>(remaining_ms);
}

void HttpServerRuntime::enter_shutdown_draining(ShutdownMachine& shutdown) {
    close_listener_for_shutdown(shutdown);
    notify_sub_reactors();

    shutdown.deadline = std::chrono::steady_clock::now() + config_.graceful_shutdown_timeout;
    shutdown.state = ShutdownState::Draining;

    std::size_t connection_count = 0;
    std::size_t pending_count = 0;
    [[maybe_unused]] const bool ignored = collect_drain_status(connection_count, pending_count);

    common::Logger::instance()
        .info("graceful shutdown started")
        .field("timeout_ms", config_.graceful_shutdown_timeout.count())
        .field("count", connection_count)
        .field("pending_count", pending_count)
        .field("next_state", "draining");
}

bool HttpServerRuntime::advance_shutdown_machine(ShutdownMachine& shutdown) {
    if (shutdown.state == ShutdownState::Serving || shutdown.state == ShutdownState::Completed) {
        return shutdown.state == ShutdownState::Completed;
    }

    notify_sub_reactors();

    std::size_t connection_count = 0;
    std::size_t pending_count = 0;
    if (collect_drain_status(connection_count, pending_count)) {
        common::Logger::instance()
            .info("graceful shutdown completed")
            .field("count", connection_count)
            .field("pending_count", pending_count)
            .field("decision", "stop_loop");
        shutdown.state = ShutdownState::Completed;
        shutdown.result = RunResult::GracefulCompleted;
        return true;
    }

    if (shutdown.state != ShutdownState::Draining || std::chrono::steady_clock::now() < shutdown.deadline) {
        return false;
    }

    common::Logger::instance()
        .warn("graceful shutdown timeout")
        .field("timeout_ms", config_.graceful_shutdown_timeout.count())
        .field("count", connection_count)
        .field("pending_count", pending_count)
        .field("decision", "force_close");
    shutdown.result = RunResult::ForcedByTimeout;
    force_close_requested_.store(true);
    notify_sub_reactors();
    shutdown.state = ShutdownState::ForceClosing;
    return true;
}

void HttpServerRuntime::close_listener_for_shutdown(ShutdownMachine& shutdown) {
    if (shutdown.listener_closed) {
        return;
    }
    shutdown.listener_closed = true;

    if (main_reactor_ == nullptr) {
        return;
    }

    if (main_reactor_->close_listener_for_shutdown()) {
        listening_port_.store(0);
    }
}

bool HttpServerRuntime::collect_drain_status(std::size_t& connection_count, std::size_t& pending_count) const {
    connection_count = 0;
    pending_count = 0;
    if (sub_reactor_pool_ == nullptr) {
        return true;
    }
    return sub_reactor_pool_->all_drained(connection_count, pending_count);
}

void HttpServerRuntime::accept_new_connections() {
    while (lifecycle_controller_.state() == LifecycleState::Running) {
        if (main_reactor_ == nullptr) {
            common::Logger::instance().error("accept failed").field("error", "main_reactor_missing");
            return;
        }

        const net::AcceptedSocket accepted = main_reactor_->accept_one();
        if (accepted.fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!common::is_would_block(errno)) {
                const int err = errno;
                common::Logger::instance()
                    .warn("accept failed")
                    .field("fd", main_reactor_->listener_fd())
                    .field("errno", err, common::errno_message(err))
                    .field("decision", "keep_running");
            }
            return;
        }

        std::size_t tracked_connections = 0;
        if (sub_reactor_pool_ != nullptr) {
            tracked_connections = sub_reactor_pool_->tracked_connection_count();
        }
        if (tracked_connections >= config_.max_connections) {
            common::Logger::instance()
                .warn("too many connections")
                .field("count", tracked_connections)
                .field("max_connections", config_.max_connections)
                .field("peer", accepted.peer)
                .field("result", "rejected");
            common::close_fd(accepted.fd);
            continue;
        }

        if (sub_reactor_pool_ != nullptr) {
            const std::uint64_t connection_token = next_connection_token_++;
            sub_reactor_pool_->dispatch_connection(accepted, connection_token);
        } else {
            common::Logger::instance()
                .error("dispatch connection failed")
                .field("fd", accepted.fd)
                .field("peer", accepted.peer)
                .field("error", "sub_reactor_pool_missing")
                .field("next_state", "closing");
            common::close_fd(accepted.fd);
        }
    }
}

void HttpServerRuntime::dispatch_sub_request(ReactorRequestTask task) {
    if (request_dispatcher_ == nullptr) {
        common::Logger::instance()
            .error("dispatch request failed")
            .field("fd", task.fd)
            .field("reactor_id", task.reactor_id)
            .field("connection_token", task.connection_token)
            .field("error", "request_dispatcher_missing")
            .field("next_state", "closing");
        return;
    }

    request_dispatcher_->dispatch(std::move(task));
}

void HttpServerRuntime::submit_response(ReactorResponseTask task) {
    const LifecycleState state = lifecycle_controller_.state();
    if (!can_submit_response_for_state(state)) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", task.fd)
            .field("reactor_id", task.reactor_id)
            .field("connection_token", task.connection_token)
            .field("state", to_string(state));
        return;
    }

    if (sub_reactor_pool_ == nullptr) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", task.fd)
            .field("reactor_id", task.reactor_id)
            .field("connection_token", task.connection_token)
            .field("error", "sub_reactor_pool_missing");
        return;
    }

    const LifecycleState state_after_check = lifecycle_controller_.state();
    if (!can_submit_response_for_state(state_after_check)) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", task.fd)
            .field("reactor_id", task.reactor_id)
            .field("connection_token", task.connection_token)
            .field("state", to_string(state_after_check));
        return;
    }

    sub_reactor_pool_->enqueue_response(std::move(task));
}

void HttpServerRuntime::notify_main_wakeup() {
    if (main_reactor_ != nullptr) {
        main_reactor_->notify_wakeup();
    }
}

void HttpServerRuntime::notify_sub_reactors() noexcept {
    if (sub_reactor_pool_ != nullptr) {
        sub_reactor_pool_->notify_all();
    }
}

void HttpServerRuntime::disable_response_submission() noexcept {
    response_submission_enabled_.store(false);
}

bool HttpServerRuntime::can_submit_response_for_state(LifecycleState state) const noexcept {
    if (!response_submission_enabled_.load()) {
        return false;
    }
    return state == LifecycleState::Running || state == LifecycleState::Stopping;
}

void HttpServerRuntime::on_sub_reactor_fatal(std::size_t reactor_id) {
    sub_reactor_fatal_error_.store(true);
    common::Logger::instance()
        .error("sub reactor fatal")
        .field("reactor_id", reactor_id)
        .field("error", "event_loop_fatal")
        .field("decision", "stop_main_loop");
    notify_main_wakeup();
}

}  // namespace nebula::server
