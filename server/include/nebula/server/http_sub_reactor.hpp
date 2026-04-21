#ifndef NEBULA_SERVER_HTTP_SUB_REACTOR_HPP
#define NEBULA_SERVER_HTTP_SUB_REACTOR_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nebula/http/http_parser.hpp"
#include "nebula/http/http_types.hpp"
#include "nebula/net/epoll_loop.hpp"
#include "nebula/server/http_reactor_tasks.hpp"
#include "nebula/server/http_sub_reactor_callbacks.hpp"
#include "nebula/server/server_config.hpp"
#include "nebula/server/server_lifecycle_state.hpp"

namespace nebula::server {

class HttpSubReactor {
public:
    struct AcceptedConnection {
        int fd = -1;
        std::uint64_t token = 0;
        std::string peer;
    };

    using RequestDispatchFn = SubReactorRequestDispatchFn;
    using LifecycleProviderFn = SubReactorLifecycleProviderFn;
    using ForceCloseProviderFn = SubReactorForceCloseProviderFn;
    using FatalErrorFn = SubReactorFatalErrorFn;

    HttpSubReactor(std::size_t id, const ServerConfig& config, RequestDispatchFn dispatch_request,
                   LifecycleProviderFn lifecycle_provider, ForceCloseProviderFn force_close_provider,
                   FatalErrorFn fatal_error_callback);
    ~HttpSubReactor() noexcept;

    HttpSubReactor(const HttpSubReactor&) = delete;
    HttpSubReactor& operator=(const HttpSubReactor&) = delete;
    HttpSubReactor(HttpSubReactor&&) = delete;
    HttpSubReactor& operator=(HttpSubReactor&&) = delete;

    bool start();
    std::size_t shutdown();

    void request_stop();
    void notify_wakeup();
    [[nodiscard]] bool enqueue_accept(AcceptedConnection accepted);
    [[nodiscard]] bool enqueue_response(ReactorResponseTask response);

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] bool is_drained() const;
    [[nodiscard]] std::size_t connection_count() const;
    [[nodiscard]] std::size_t pending_accept_count() const;
    [[nodiscard]] std::size_t pending_response_count() const;
    [[nodiscard]] std::size_t tracked_connection_count() const;

private:
    enum class RunState : std::uint8_t {
        Drained,
        Starting,
        Running,
        StopRequested,
        ThreadExited,
    };

    struct Connection {
        int fd = -1;
        std::uint64_t token = 0;
        std::string peer;
        std::string read_buffer;
        std::string write_buffer;
        bool close_after_write = false;
        bool processing = false;
        std::optional<std::chrono::steady_clock::time_point> active_request_started_at;
        http::HttpRequestParseContext parse_context;
        std::chrono::steady_clock::time_point last_active;
    };
    using PendingResponse = ReactorResponseTask;

    [[nodiscard]] LifecycleState current_lifecycle_state() const;
    [[nodiscard]] int load_wakeup_fd() const;
    [[nodiscard]] static bool should_keep_running_for_state(LifecycleState state);
    [[nodiscard]] bool handle_force_close_if_requested();
    [[nodiscard]] bool wait_and_process_events();
    void reset_runtime_state_for_start();
    std::size_t clear_pending_queues();
    void close_all_connections();

    void run_loop();
    void handle_run_loop_exception(const char* error) noexcept;
    void notify_fatal_error_safely() noexcept;
    void begin_graceful_shutdown(bool& shutdown_started);
    std::size_t close_idle_connections_for_shutdown();
    [[nodiscard]] bool has_inflight_work();

    void process_ready_events(int ready_count);
    void drain_pending_accepts();
    void drain_pending_responses();
    void handle_client_event(int fd, std::uint32_t events);
    void handle_readable(Connection& connection);
    void parse_next_request_if_ready(Connection& connection);
    [[nodiscard]] bool append_read_data(Connection& connection, const char* data, std::size_t read_n,
                                        std::size_t pending_limit);
    void handle_pending_bytes_exceeded(Connection& connection, std::size_t pending_limit);
    [[nodiscard]] bool handle_recv_error(Connection& connection, int err, bool& should_break);
    void handle_writable(Connection& connection);
    void schedule_request(Connection& connection, http::HttpRequest request, std::size_t request_bytes,
                          std::chrono::steady_clock::time_point request_started_at);
    void parse_next_request(Connection& connection);

    void enqueue_error_response(int fd, std::uint64_t token, http::HttpStatus status, std::string_view error_message,
                                bool close_after_write, bool suppress_body, std::string request_line,
                                std::size_t request_bytes, std::chrono::steady_clock::time_point request_started_at);
    void close_with_response_enqueue_error(const char* event, int fd, std::uint64_t token, const char* error,
                                           std::string_view close_reason);
    void apply_response_to_connection(Connection& connection, const http::HttpResponse& response,
                                      bool close_after_write, bool suppress_body, std::string_view request_line,
                                      std::size_t request_bytes,
                                      std::chrono::steady_clock::time_point request_started_at);

    static void drain_wakeup(int wakeup_fd);
    void close_connection(int fd, std::string_view close_reason);
    void sweep_idle_connections();

    std::size_t id_ = 0;
    const ServerConfig& config_;
    RequestDispatchFn dispatch_request_;
    LifecycleProviderFn lifecycle_provider_;
    ForceCloseProviderFn force_close_provider_;
    FatalErrorFn fatal_error_callback_;

    net::EpollLoop epoll_;
    std::vector<epoll_event> events_;
    std::unordered_map<int, Connection> connections_;

    mutable std::mutex pending_accepts_mutex_;
    std::vector<AcceptedConnection> pending_accepts_;

    mutable std::mutex pending_responses_mutex_;
    std::vector<ReactorResponseTask> pending_responses_;

    mutable std::mutex wakeup_mutex_;
    int wakeup_fd_ = -1;

    std::atomic<RunState> run_state_ = RunState::Drained;
    std::atomic<std::size_t> connection_count_ = 0;
    std::atomic<std::size_t> pending_accept_count_ = 0;
    std::atomic<std::size_t> pending_response_count_ = 0;

    std::thread thread_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_SUB_REACTOR_HPP
