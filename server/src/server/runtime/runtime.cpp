#include "nebula/server/runtime/runtime.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/posix.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/net/address_discovery.hpp"
#include "nebula/server/lifecycle/signal_handler.hpp"
#include "nebula/server/reactor/constants.hpp"
#include "nebula/server/reactor/main_reactor.hpp"
#include "nebula/server/reactor/sub_reactor_pool.hpp"
#include "nebula/server/runtime/http_request_dispatcher.hpp"

namespace nebula::server {

namespace {

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
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("fallback", "localhost_and_loopback_only");
    }
}

void log_runtime_exception(const char* event, LifecycleState state, const char* error) noexcept {
    common::Logger::instance()
        .error(event != nullptr ? event : "runtime exception")
        .field("state", to_string(state))
        .field("error", error != nullptr ? error : "unknown");
}

}  // namespace

ServerRuntime::ServerRuntime(ServerConfig server_config, ServerTimeoutConfig timeouts, http::HttpLimitsConfig limits,
                             std::shared_ptr<http::Router> router, std::shared_ptr<auth::AuthService> auth_service)
    : server_config_(server_config.normalize()),
      timeouts_(timeouts),
      limits_(limits),
      signal_handler_(server_config_.manage_signals ? std::make_unique<SignalHandler>() : nullptr),
      router_(std::move(router)),
      auth_service_(std::move(auth_service)),
      thread_pool_(server_config_.worker_thread_count) {
    request_dispatcher_ = std::make_unique<HttpRequestDispatcher>(
        router_, auth_service_, thread_pool_, [this](ReactorResponseTask task) { submit_response(std::move(task)); });

    main_reactor_ = std::make_unique<MainReactor>();

    sub_reactor_pool_ = std::make_unique<SubReactorPool>(
        server_config_, timeouts_, limits_,
        [this](ReactorRequestTask task) { request_dispatcher_->dispatch(std::move(task)); },
        [this]() { return lifecycle_controller_.state(); }, [this]() { return force_close_requested_.load(); },
        [this](std::size_t reactor_id) { on_sub_reactor_fatal(reactor_id); });
}

ServerRuntime::~ServerRuntime() noexcept {
    try {
        request_stop();
        thread_pool_.stop();
        shutdown_runtime();
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("server runtime destructor cleanup failed")
            .field("error", e.what())
            .field("decision", "ignore");
    } catch (...) {
        common::Logger::instance()
            .error("server runtime destructor cleanup failed")
            .field("error", "unknown")
            .field("decision", "ignore");
    }
}

RunResult ServerRuntime::run() noexcept {
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
            response_submission_enabled_.store(false);
            force_close_requested_.store(true);
            main_reactor_->notify_wakeup();
            sub_reactor_pool_->notify_all();
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
            .field("backlog", server_config_.backlog)
            .field("max_connections", server_config_.max_connections)
            .field("sub_reactor_count", server_config_.sub_reactor_count)
            .field("worker_thread_count", server_config_.worker_thread_count)
            .field("timeout_ms", timeouts_.read_timeout.count());

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

void ServerRuntime::request_stop() noexcept {
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

        main_reactor_->notify_wakeup();
        sub_reactor_pool_->notify_all();
    } catch (const std::exception& e) {
        log_runtime_exception("runtime stop exception", lifecycle_controller_.state(), e.what());
    } catch (...) {
        log_runtime_exception("runtime stop exception", lifecycle_controller_.state(), "unknown");
    }
}

bool ServerRuntime::is_running() const noexcept {
    return lifecycle_controller_.state() == LifecycleState::Running;
}

std::uint16_t ServerRuntime::listening_port() const noexcept {
    return listening_port_.load();
}

bool ServerRuntime::init_runtime() {
    force_close_requested_.store(false);
    sub_reactor_fatal_error_.store(false);

    if (!main_reactor_->open(server_config_.port, server_config_.backlog)) {
        return false;
    }

    if (!sub_reactor_pool_->start()) {
        main_reactor_->close();
        return false;
    }

    listening_port_.store(main_reactor_->listening_port());
    return true;
}

void ServerRuntime::shutdown_runtime() {
    response_submission_enabled_.store(false);
    force_close_requested_.store(true);

    sub_reactor_pool_->shutdown();
    main_reactor_->close();
    listening_port_.store(0);

    force_close_requested_.store(false);
    sub_reactor_fatal_error_.store(false);
}

RunResult ServerRuntime::run_event_loop() {
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

        if (!main_reactor_->wait_and_process_events([this]() { accept_new_connections(); },
                                                    compute_wait_timeout_ms(shutdown))) {
            response_submission_enabled_.store(false);
            return RunResult::FatalError;
        }

        if (sub_reactor_fatal_error_.load()) {
            common::Logger::instance()
                .error("sub reactor failed")
                .field("error", "sub_reactor_event_loop_fatal")
                .field("decision", "stop_loop");
            response_submission_enabled_.store(false);
            return RunResult::FatalError;
        }

        if (lifecycle_controller_.state() == LifecycleState::Stopping && advance_shutdown_machine(shutdown)) {
            break;
        }
    }

    response_submission_enabled_.store(false);
    return shutdown.result;
}

int ServerRuntime::compute_wait_timeout_ms(const ShutdownMachine& shutdown) {
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
    if (std::cmp_greater_equal(remaining_ms, kEventWaitTimeoutMs)) {
        return kEventWaitTimeoutMs;
    }
    return static_cast<int>(remaining_ms);
}

void ServerRuntime::enter_shutdown_draining(ShutdownMachine& shutdown) {
    close_listener_for_shutdown(shutdown);
    sub_reactor_pool_->notify_all();

    shutdown.deadline = std::chrono::steady_clock::now() + timeouts_.graceful_shutdown_timeout;
    shutdown.state = ShutdownState::Draining;

    std::size_t connection_count = 0;
    std::size_t pending_count = 0;
    collect_drain_status(connection_count, pending_count);

    common::Logger::instance()
        .info("graceful shutdown started")
        .field("timeout_ms", timeouts_.graceful_shutdown_timeout.count())
        .field("count", connection_count)
        .field("pending_count", pending_count)
        .field("next_state", "draining");
}

bool ServerRuntime::advance_shutdown_machine(ShutdownMachine& shutdown) {
    if (shutdown.state == ShutdownState::Serving || shutdown.state == ShutdownState::Completed) {
        return shutdown.state == ShutdownState::Completed;
    }

    sub_reactor_pool_->notify_all();

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
        .field("timeout_ms", timeouts_.graceful_shutdown_timeout.count())
        .field("count", connection_count)
        .field("pending_count", pending_count)
        .field("decision", "force_close");
    shutdown.result = RunResult::ForcedByTimeout;
    force_close_requested_.store(true);
    sub_reactor_pool_->notify_all();
    shutdown.state = ShutdownState::ForceClosing;
    return true;
}

void ServerRuntime::close_listener_for_shutdown(ShutdownMachine& shutdown) {
    if (shutdown.listener_closed) {
        return;
    }
    shutdown.listener_closed = true;

    if (main_reactor_->close_listener_for_shutdown()) {
        listening_port_.store(0);
    }
}

bool ServerRuntime::collect_drain_status(std::size_t& connection_count, std::size_t& pending_count) const {
    connection_count = 0;
    pending_count = 0;
    return sub_reactor_pool_->all_drained(connection_count, pending_count);
}

void ServerRuntime::accept_new_connections() {
    while (lifecycle_controller_.state() == LifecycleState::Running) {
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
                    .field("errno", err)
                    .field("error", common::errno_message(err))
                    .field("decision", "keep_running");
            }
            return;
        }

        const std::size_t tracked_connections = sub_reactor_pool_->tracked_connection_count();
        if (tracked_connections >= server_config_.max_connections) {
            common::Logger::instance()
                .warn("too many connections")
                .field("count", tracked_connections)
                .field("max_connections", server_config_.max_connections)
                .field("peer", accepted.peer)
                .field("result", "rejected");
            common::close_fd(accepted.fd);
            continue;
        }

        const std::uint64_t connection_token = next_connection_token_++;
        sub_reactor_pool_->dispatch_connection(accepted, connection_token);
    }
}

void ServerRuntime::submit_response(ReactorResponseTask task) {
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

bool ServerRuntime::can_submit_response_for_state(LifecycleState state) const noexcept {
    if (!response_submission_enabled_.load()) {
        return false;
    }
    return state == LifecycleState::Running || state == LifecycleState::Stopping;
}

void ServerRuntime::on_sub_reactor_fatal(std::size_t reactor_id) {
    sub_reactor_fatal_error_.store(true);
    common::Logger::instance()
        .error("sub reactor fatal")
        .field("reactor_id", reactor_id)
        .field("error", "event_loop_fatal")
        .field("decision", "stop_main_loop");
    main_reactor_->notify_wakeup();
}

}  // namespace nebula::server
