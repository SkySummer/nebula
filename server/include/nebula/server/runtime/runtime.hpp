#ifndef NEBULA_SERVER_RUNTIME_RUNTIME_HPP
#define NEBULA_SERVER_RUNTIME_RUNTIME_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "nebula/auth/application/service.hpp"
#include "nebula/common/runtime/thread_pool.hpp"
#include "nebula/http/config/http_limits_config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/server/config/config.hpp"
#include "nebula/server/config/timeouts_config.hpp"
#include "nebula/server/lifecycle/lifecycle_controller.hpp"
#include "nebula/server/lifecycle/signal_handler.hpp"
#include "nebula/server/reactor/main_reactor.hpp"
#include "nebula/server/reactor/sub_reactor_pool.hpp"
#include "nebula/server/reactor/tasks.hpp"
#include "nebula/server/runtime/http_request_dispatcher.hpp"
#include "nebula/server/runtime/run_result.hpp"

namespace nebula::server {

class ServerBuilder;

class ServerRuntime {
public:
    ~ServerRuntime() noexcept;

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;
    ServerRuntime(ServerRuntime&&) = delete;
    ServerRuntime& operator=(ServerRuntime&&) = delete;

    RunResult run() noexcept;
    void request_stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t listening_port() const noexcept;

private:
    friend class ServerBuilder;

    ServerRuntime(ServerConfig server_config, ServerTimeoutConfig timeouts, http::HttpLimitsConfig limits,
                  std::shared_ptr<http::Router> router, std::shared_ptr<auth::AuthService> auth_service);

    enum class ShutdownState : std::uint8_t {
        Serving,
        Draining,
        ForceClosing,
        Completed,
    };

    struct ShutdownMachine {
        ShutdownState state = ShutdownState::Serving;
        std::chrono::steady_clock::time_point deadline;
        RunResult result = RunResult::GracefulCompleted;
        bool listener_closed = false;
    };

    bool init_runtime();
    void shutdown_runtime();
    [[nodiscard]] RunResult run_event_loop();
    [[nodiscard]] static int compute_wait_timeout_ms(const ShutdownMachine& shutdown);
    void enter_shutdown_draining(ShutdownMachine& shutdown);
    [[nodiscard]] bool advance_shutdown_machine(ShutdownMachine& shutdown);
    void close_listener_for_shutdown(ShutdownMachine& shutdown);
    bool collect_drain_status(std::size_t& connection_count, std::size_t& pending_count) const;

    void accept_new_connections();
    void submit_response(ReactorResponseTask task);

    void on_sub_reactor_fatal(std::size_t reactor_id);
    [[nodiscard]] bool can_submit_response_for_state(LifecycleState state) const noexcept;

    ServerConfig server_config_;
    ServerTimeoutConfig timeouts_;
    http::HttpLimitsConfig limits_;
    std::unique_ptr<SignalHandler> signal_handler_;
    std::unique_ptr<MainReactor> main_reactor_;
    std::unique_ptr<SubReactorPool> sub_reactor_pool_;
    std::atomic_bool force_close_requested_ = false;
    std::atomic_bool sub_reactor_fatal_error_ = false;

    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
    common::ThreadPool thread_pool_;
    std::unique_ptr<HttpRequestDispatcher> request_dispatcher_;
    std::uint64_t next_connection_token_ = 1;

    ServerLifecycleController lifecycle_controller_;
    std::atomic<std::uint16_t> listening_port_ = 0;
    std::atomic_bool response_submission_enabled_ = false;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_RUNTIME_RUNTIME_HPP
