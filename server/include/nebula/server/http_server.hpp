#ifndef NEBULA_SERVER_HTTP_SERVER_HPP
#define NEBULA_SERVER_HTTP_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nebula/common/thread_pool.hpp"
#include "nebula/http/http_types.hpp"
#include "nebula/http/router.hpp"
#include "nebula/net/epoll_loop.hpp"
#include "nebula/net/listener.hpp"
#include "nebula/server/server_config.hpp"

namespace nebula::server {

class SignalHandler;

class HttpServer {
public:
    enum class RunResult : std::uint8_t {
        StartRejected,
        StartCanceled,
        StartFailed,
        GracefulCompleted,
        ForcedByTimeout,
        FatalError,
        CleanupFailed,
    };

    explicit HttpServer(ServerConfig config = {});
    ~HttpServer() noexcept;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    bool add_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept;
    bool mod_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept;
    bool del_route(http::HttpMethod method, const std::string& path) noexcept;

    RunResult start() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] std::uint16_t listening_port() const;
    [[nodiscard]] static bool is_successful_run_result(RunResult result);
    [[nodiscard]] static std::string_view to_string(RunResult result);

private:
    enum class LifecycleState : std::uint8_t {
        Idle,
        Starting,
        Running,
        Stopping,
    };

    [[nodiscard]] static std::string_view to_string(LifecycleState state);

    struct Connection {
        int fd = -1;
        std::uint64_t token = 0;
        std::string peer;
        std::string read_buffer;
        std::string write_buffer;
        bool close_after_write = false;
        bool processing = false;
        std::chrono::steady_clock::time_point last_active;
    };

    struct PendingResponse {
        int fd = -1;
        std::uint64_t token = 0;
        http::HttpResponse response;
        bool close_after_write = false;
        bool suppress_body = false;
        std::string request_line;
        std::size_t request_bytes = 0;
        std::chrono::steady_clock::time_point request_started_at;
    };

    bool init_runtime();
    void shutdown_runtime();
    [[nodiscard]] RunResult run_event_loop();
    void begin_graceful_shutdown(bool& shutdown_started, std::chrono::steady_clock::time_point& shutdown_deadline,
                                 bool& listener_closed_for_shutdown);
    void close_listener_for_shutdown(bool& listener_closed_for_shutdown);
    [[nodiscard]] std::size_t close_idle_connections_for_shutdown();
    [[nodiscard]] std::size_t pending_response_count();
    [[nodiscard]] bool has_inflight_work(std::size_t& pending_count);
    void process_ready_events(int ready_count);

    void accept_new_connections();
    void handle_client_event(int fd, std::uint32_t events);
    void handle_readable(Connection& connection);
    void handle_writable(Connection& connection);
    void schedule_request(Connection& connection, http::HttpRequest request, std::size_t request_bytes);
    void parse_next_request(Connection& connection);
    void submit_response(int fd, std::uint64_t connection_token, http::HttpResponse response, bool close_after_write,
                         bool suppress_body, std::string request_line, std::size_t request_bytes,
                         std::chrono::steady_clock::time_point request_started_at);
    void submit_error_response(int fd, std::uint64_t connection_token, http::HttpStatus status, std::string body,
                               bool close_after_write, bool suppress_body, std::string request_line,
                               std::size_t request_bytes, std::chrono::steady_clock::time_point request_started_at);
    void apply_response_to_connection(Connection& connection, const http::HttpResponse& response,
                                      bool close_after_write, bool suppress_body, std::string_view request_line,
                                      std::size_t request_bytes,
                                      std::chrono::steady_clock::time_point request_started_at);

    static void drain_wakeup(int wakeup_fd);
    void drain_pending_responses();
    void notify_wakeup();
    [[nodiscard]] int load_wakeup_fd() const;

    void close_connection(int fd, std::string_view close_reason);
    void sweep_idle_connections();

    http::HttpResponse dispatch_request(const http::HttpRequest& request);

    ServerConfig config_;
    std::unique_ptr<SignalHandler> signal_handler_;
    net::Listener listener_;
    net::EpollLoop epoll_;
    std::vector<epoll_event> events_;
    std::unordered_map<int, Connection> connections_;

    std::mutex pending_mutex_;
    std::vector<PendingResponse> pending_responses_;

    mutable std::mutex wakeup_mutex_;
    int wakeup_fd_ = -1;

    http::Router router_;
    common::ThreadPool thread_pool_;
    std::uint64_t next_connection_token_ = 1;

    std::atomic<LifecycleState> lifecycle_state_ = LifecycleState::Idle;
    std::atomic<std::uint16_t> listening_port_ = 0;
    std::atomic<bool> prestart_stop_requested_ = false;
    std::atomic<bool> response_submission_enabled_ = false;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_SERVER_HPP
