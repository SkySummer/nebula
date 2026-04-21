#ifndef NEBULA_SERVER_SERVER_RUNTIME_HPP
#define NEBULA_SERVER_SERVER_RUNTIME_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "nebula/app/server_config.hpp"
#include "nebula/auth/auth_service.hpp"
#include "nebula/common/thread_pool.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/http_request_dispatcher.hpp"
#include "nebula/server/main_reactor.hpp"
#include "nebula/server/reactor_tasks.hpp"
#include "nebula/server/run_result.hpp"
#include "nebula/server/server_lifecycle_controller.hpp"
#include "nebula/server/signal_handler.hpp"
#include "nebula/server/sub_reactor_pool.hpp"

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

    ServerRuntime(app::ServerConfig config, std::shared_ptr<http::Router> router,
                  std::shared_ptr<auth::AuthService> auth_service);

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
    void dispatch_sub_request(ReactorRequestTask task);
    void submit_response(ReactorResponseTask task);

    void notify_main_wakeup();
    void on_sub_reactor_fatal(std::size_t reactor_id);
    void notify_sub_reactors() noexcept;
    void disable_response_submission() noexcept;
    [[nodiscard]] bool can_submit_response_for_state(LifecycleState state) const noexcept;

    app::ServerConfig config_;
    std::unique_ptr<SignalHandler> signal_handler_;
    std::unique_ptr<MainReactor> main_reactor_;
    std::unique_ptr<SubReactorPool> sub_reactor_pool_;
    std::atomic<bool> force_close_requested_ = false;
    std::atomic<bool> sub_reactor_fatal_error_ = false;

    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
    common::ThreadPool thread_pool_;
    std::unique_ptr<HttpRequestDispatcher> request_dispatcher_;
    std::uint64_t next_connection_token_ = 1;

    ServerLifecycleController lifecycle_controller_;
    std::atomic<std::uint16_t> listening_port_ = 0;
    std::atomic<bool> response_submission_enabled_ = false;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SERVER_RUNTIME_HPP
