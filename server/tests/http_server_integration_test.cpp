#include "nebula/server/http_server.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <future>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nebula/common/logger.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::testsupport::capture_stderr;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_not_contains;
using nebula::testsupport::expect_true;
using RunResult = nebula::server::HttpServer::RunResult;

bool is_nonfatal_run_result(RunResult result) {
    return result == RunResult::StartCanceled || result == RunResult::GracefulCompleted ||
           result == RunResult::ForcedByTimeout;
}

sockaddr* as_sockaddr(sockaddr_in& addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&addr);
}

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent_n = ::send(fd, data.data() + offset, data.size() - offset, 0);
        if (sent_n > 0) {
            offset += static_cast<std::size_t>(sent_n);
            continue;
        }

        if (sent_n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

int connect_localhost(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, as_sockaddr(addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

void close_with_reset(int fd) {
    linger reset_linger{};
    reset_linger.l_onoff = 1;
    reset_linger.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger));
    ::close(fd);
}

std::size_t parse_content_length(std::string_view header) {
    const std::string key = "content-length:";
    std::string lower(header);
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    const std::size_t pos = lower.find(key);
    if (pos == std::string::npos) {
        return 0;
    }

    const std::size_t value_begin = pos + key.size();
    const std::size_t line_end = lower.find("\r\n", value_begin);
    std::string value =
        lower.substr(value_begin, line_end == std::string::npos ? std::string::npos : line_end - value_begin);

    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    if (start >= end) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoull(value.substr(start, end - start)));
}

std::string read_one_response(int fd, std::string& carry_buffer) {
    std::vector<char> tmp(4096U);

    while (true) {
        const std::size_t header_end = carry_buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::size_t body_len = parse_content_length(carry_buffer.substr(0, header_end + 2U));
            const std::size_t total_len = header_end + 4U + body_len;
            if (carry_buffer.size() >= total_len) {
                const std::string response = carry_buffer.substr(0, total_len);
                carry_buffer.erase(0, total_len);
                return response;
            }
        }

        const ssize_t read_n = ::recv(fd, tmp.data(), tmp.size(), 0);
        if (read_n > 0) {
            carry_buffer.append(tmp.data(), static_cast<std::size_t>(read_n));
            continue;
        }

        if (read_n == 0) {
            return carry_buffer;
        }

        if (errno == EINTR) {
            continue;
        }

        return carry_buffer;
    }
}

std::string read_until_close(int fd) {
    std::string out;
    std::vector<char> tmp(4096U);
    while (true) {
        const ssize_t read_n = ::recv(fd, tmp.data(), tmp.size(), 0);
        if (read_n > 0) {
            out.append(tmp.data(), static_cast<std::size_t>(read_n));
            continue;
        }
        if (read_n == 0) {
            return out;
        }
        if (errno == EINTR) {
            continue;
        }
        return out;
    }
}

int poll_socket_events(int fd, std::chrono::milliseconds timeout) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN | POLLERR | POLLHUP;

    while (true) {
        const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (ready > 0) {
            return descriptor.revents;
        }
        if (ready == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return descriptor.revents;
    }
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }

    std::size_t count = 0;
    std::size_t pos = 0;
    while (true) {
        pos = text.find(needle, pos);
        if (pos == std::string_view::npos) {
            return count;
        }
        ++count;
        pos += needle.size();
    }
}

void wait_until_server_ready(nebula::server::HttpServer& server) {
    using namespace std::chrono_literals;

    for (int idx = 0; idx < 200; ++idx) {
        if (server.is_running() && server.listening_port() > 0) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }

    nebula::testsupport::fail("server did not become ready in time");
}

class ServerThreadGuard {
public:
    ServerThreadGuard(nebula::server::HttpServer& server, std::thread& thread) : server_(server), thread_(thread) {}

    ServerThreadGuard(const ServerThreadGuard&) = delete;
    ServerThreadGuard& operator=(const ServerThreadGuard&) = delete;
    ServerThreadGuard(ServerThreadGuard&&) = delete;
    ServerThreadGuard& operator=(ServerThreadGuard&&) = delete;

    ~ServerThreadGuard() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    nebula::server::HttpServer& server_;
    std::thread& thread_;
};

class SignalMaskGuard {
public:
    SignalMaskGuard() : valid_(::pthread_sigmask(SIG_SETMASK, nullptr, &saved_mask_) == 0) {}

    SignalMaskGuard(const SignalMaskGuard&) = delete;
    SignalMaskGuard& operator=(const SignalMaskGuard&) = delete;
    SignalMaskGuard(SignalMaskGuard&&) = delete;
    SignalMaskGuard& operator=(SignalMaskGuard&&) = delete;

    ~SignalMaskGuard() {
        if (valid_) {
            ::pthread_sigmask(SIG_SETMASK, &saved_mask_, nullptr);
        }
    }

    [[nodiscard]] bool valid() const {
        return valid_;
    }

    [[nodiscard]] const sigset_t& saved_mask() const {
        return saved_mask_;
    }

private:
    sigset_t saved_mask_{};
    bool valid_ = false;
};

class SignalActionGuard {
public:
    explicit SignalActionGuard(int signal) : signal_(signal) {
        const int read_result = ::sigaction(signal_, nullptr, &saved_action_);
        if (read_result != 0) {
            return;
        }

        has_saved_action_ = true;
        struct sigaction action{};
        action.sa_handler = +[](int) {};
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        installed_ = (::sigaction(signal_, &action, nullptr) == 0);
    }

    SignalActionGuard(const SignalActionGuard&) = delete;
    SignalActionGuard& operator=(const SignalActionGuard&) = delete;
    SignalActionGuard(SignalActionGuard&&) = delete;
    SignalActionGuard& operator=(SignalActionGuard&&) = delete;

    ~SignalActionGuard() {
        if (has_saved_action_) {
            ::sigaction(signal_, &saved_action_, nullptr);
        }
    }

    [[nodiscard]] bool installed() const {
        return installed_;
    }

private:
    int signal_;
    struct sigaction saved_action_{};
    bool has_saved_action_ = false;
    bool installed_ = false;
};

void test_healthz_endpoint() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "healthz should return 200");
    expect_contains(response, R"({"status":"ok"})", "healthz should return expected json body");
}

void test_signal_mask_restored_after_http_server_destroyed() {
    SignalMaskGuard signal_mask_guard;
    expect_true(signal_mask_guard.valid(), "capture initial signal mask should succeed");

    const sigset_t before_mask = signal_mask_guard.saved_mask();

    {
        nebula::server::ServerConfig config;
        config.port = 0;
        config.worker_threads = 1;
        nebula::server::HttpServer server(config);
    }

    sigset_t after_mask{};
    const int read_mask_result = ::pthread_sigmask(SIG_SETMASK, nullptr, &after_mask);
    expect_true(read_mask_result == 0, "capture signal mask after server destroy should succeed");

    const int before_sigint = ::sigismember(&before_mask, SIGINT);
    const int after_sigint = ::sigismember(&after_mask, SIGINT);
    const int before_sigterm = ::sigismember(&before_mask, SIGTERM);
    const int after_sigterm = ::sigismember(&after_mask, SIGTERM);

    expect_true(before_sigint >= 0 && after_sigint >= 0, "sigismember for SIGINT should succeed");
    expect_true(before_sigterm >= 0 && after_sigterm >= 0, "sigismember for SIGTERM should succeed");
    expect_true(before_sigint == after_sigint, "SIGINT mask state should be restored after server destruction");
    expect_true(before_sigterm == after_sigterm, "SIGTERM mask state should be restored after server destruction");
}

void test_signal_shutdown_remains_available_after_restart() {
    using namespace std::chrono_literals;

    SignalActionGuard sigint_guard(SIGINT);
    SignalActionGuard sigterm_guard(SIGTERM);
    expect_true(sigint_guard.installed(), "install SIGINT temporary handler should succeed");
    expect_true(sigterm_guard.installed(), "install SIGTERM temporary handler should succeed");

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    const auto run_cycle_and_stop_with_sigterm = [&server](std::string_view cycle_name) {
        std::future<RunResult> start_future = std::async(std::launch::async, [&server]() { return server.start(); });

        wait_until_server_ready(server);

        const int raise_result = ::kill(::getpid(), SIGTERM);
        expect_true(raise_result == 0, "send SIGTERM should succeed");

        const bool stopped_by_signal = (start_future.wait_for(2s) == std::future_status::ready);
        if (!stopped_by_signal) {
            server.stop();
        }

        expect_true(stopped_by_signal, std::string(cycle_name) + " cycle should stop after SIGTERM");
        expect_true(start_future.wait_for(2s) == std::future_status::ready,
                    std::string(cycle_name) + " cycle should return after stop");

        const RunResult result = start_future.get();
        expect_true(is_nonfatal_run_result(result), std::string(cycle_name) + " cycle should exit without fatal error");
        expect_true(!server.is_running(), std::string(cycle_name) + " cycle should leave server stopped");
    };

    run_cycle_and_stop_with_sigterm("first");
    run_cycle_and_stop_with_sigterm("second");
}

void test_signal_shutdown_with_preexisting_unmasked_thread() {
    using namespace std::chrono_literals;

    SignalActionGuard sigint_guard(SIGINT);
    SignalActionGuard sigterm_guard(SIGTERM);
    expect_true(sigint_guard.installed(), "install SIGINT temporary handler should succeed");
    expect_true(sigterm_guard.installed(), "install SIGTERM temporary handler should succeed");

    std::atomic<bool> helper_ready = false;
    std::jthread helper_thread([&helper_ready](const std::stop_token& stop_token) {
        helper_ready.store(true);
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(10ms);
        }
    });

    for (int idx = 0; idx < 50 && !helper_ready.load(); ++idx) {
        std::this_thread::sleep_for(10ms);
    }
    expect_true(helper_ready.load(), "preexisting helper thread should start");

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::future<RunResult> start_future = std::async(std::launch::async, [&server]() { return server.start(); });
    wait_until_server_ready(server);

    const int raise_result = ::kill(::getpid(), SIGTERM);
    expect_true(raise_result == 0, "send SIGTERM should succeed");

    const bool stopped_by_signal = (start_future.wait_for(2s) == std::future_status::ready);
    if (!stopped_by_signal) {
        server.stop();
    }

    expect_true(stopped_by_signal, "server should stop after SIGTERM with preexisting unmasked thread");
    expect_true(start_future.wait_for(2s) == std::future_status::ready,
                "server start should return after SIGTERM stop");

    const RunResult result = start_future.get();
    expect_true(is_nonfatal_run_result(result), "server should exit without fatal error after SIGTERM");
    expect_true(!server.is_running(), "server should leave stopped state after SIGTERM");

    helper_thread.request_stop();
}

void test_healthz_endpoint_absolute_form() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET http://localhost/healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "absolute-form healthz should return 200");
    expect_contains(response, R"({"status":"ok"})", "absolute-form healthz should return expected json body");
}

void test_healthz_endpoint_with_query() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz?ready=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "healthz with query should return 200");
    expect_contains(response, R"({"status":"ok"})", "healthz with query should return expected json body");
}

void test_root_redirects_to_healthz() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 307 Temporary Redirect", "root should redirect to healthz");
    expect_contains(response, "Location: /healthz", "root redirect should include location header");
}

void test_head_method_suppresses_body() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    const std::string route_body = "head-body-should-not-appear";
    expect_true(server.add_route(nebula::http::HttpMethod::Head, "/headz",
                                 [route_body](const nebula::http::HttpRequest&) {
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = route_body;
                                     return response;
                                 }),
                "add head route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "HEAD /headz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    const std::string response = read_until_close(fd);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "head route should return 200");
    expect_not_contains(response, route_body, "head response must not include body bytes");

    const std::size_t header_end = response.find("\r\n\r\n");
    expect_true(header_end != std::string::npos, "head response should contain header terminator");

    const std::size_t content_length = parse_content_length(response.substr(0, header_end + 2U));
    expect_true(content_length == route_body.size(), "head response should keep content-length of handler body");

    const std::size_t actual_body_len = response.size() - (header_end + 4U);
    expect_true(actual_body_len == 0U, "head response should have empty payload");
}

void test_echo_endpoint() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string body = R"({"message":"hi"})";
    const std::string request = std::string("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ") +
                                std::to_string(body.size()) +
                                "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + body;
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "echo should return 200");
    expect_contains(response, body, "echo should return original body");
}

void test_request_completed_log_uses_raw_request_line() {
    const nebula::testsupport::TempDir log_dir("nebula-http-server-request-log");
    const std::string request = "GET http://localhost/healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    const std::string stderr_text = capture_stderr([&]() {
        nebula::common::Logger::instance().init(nebula::common::LogLevel::Info, log_dir.path(), true);

        nebula::server::ServerConfig config;
        config.port = 0;
        config.worker_threads = 2;
        nebula::server::HttpServer server(config);

        std::thread server_thread([&server]() { server.start(); });
        ServerThreadGuard server_guard(server, server_thread);
        wait_until_server_ready(server);

        const int fd = connect_localhost(server.listening_port());
        expect_true(fd >= 0, "connect should succeed");
        expect_true(send_all(fd, request), "send request should succeed");

        std::string carry;
        const std::string response = read_one_response(fd, carry);
        ::close(fd);
        expect_contains(response, "HTTP/1.1 200 OK", "request should be served");
    });

    nebula::common::Logger::instance().init(nebula::common::LogLevel::Warning, log_dir.path(), false);

    expect_contains(stderr_text, "[INFO] request completed: fd=", "request completed log should be emitted");
    expect_contains(stderr_text, "request=\"GET http://localhost/healthz HTTP/1.1\"",
                    "request field should use raw request line");
    expect_contains(stderr_text, std::string("request_bytes=") + std::to_string(request.size()),
                    "request bytes should match raw request size");
    expect_contains(stderr_text, "status=200 (OK)", "status field should include status text");
    expect_contains(stderr_text, "response_bytes=", "response bytes field should be logged");
    expect_contains(stderr_text, "latency_ms=", "latency field should be logged");
}

void test_keep_alive_two_requests() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request1 = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    expect_true(send_all(fd, request1), "first request should send");

    std::string carry;
    const std::string response1 = read_one_response(fd, carry);
    expect_contains(response1, "HTTP/1.1 200 OK", "first keep-alive response should be 200");

    const std::string body = R"({"second":true})";
    const std::string request2 = std::string("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ") +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    expect_true(send_all(fd, request2), "second request should send");

    const std::string response2 = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response2, "HTTP/1.1 200 OK", "second keep-alive response should be 200");
    expect_contains(response2, body, "second keep-alive response should echo body");
}

void test_client_reset_during_response_keeps_server_running() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow-reset",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(150ms);
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow-reset";
                                     return response;
                                 }),
                "add slow-reset route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int reset_fd = connect_localhost(server.listening_port());
    expect_true(reset_fd >= 0, "connect for reset scenario should succeed");

    const std::string slow_request = "GET /slow-reset HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(reset_fd, slow_request), "slow-reset request should send");
    close_with_reset(reset_fd);

    std::this_thread::sleep_for(250ms);
    expect_true(server.is_running(), "server should keep running after peer reset");

    const int probe_fd = connect_localhost(server.listening_port());
    expect_true(probe_fd >= 0, "probe connect should succeed");

    const std::string probe_request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(probe_fd, probe_request), "probe request should send");
    std::string carry;
    const std::string probe_response = read_one_response(probe_fd, carry);
    ::close(probe_fd);

    expect_contains(probe_response, "HTTP/1.1 200 OK", "server should keep serving after peer reset");
}

void test_stale_async_response_not_delivered_to_new_connection() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow-fd-reuse",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(250ms);
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow-fd-reuse";
                                     return response;
                                 }),
                "add slow-fd-reuse route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int stale_fd = connect_localhost(server.listening_port());
    expect_true(stale_fd >= 0, "connect stale client should succeed");

    const std::string stale_request = "GET /slow-fd-reuse HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(stale_fd, stale_request), "slow-fd-reuse request should send");
    close_with_reset(stale_fd);

    std::this_thread::sleep_for(30ms);

    const int fresh_fd = connect_localhost(server.listening_port());
    expect_true(fresh_fd >= 0, "connect fresh client should succeed");

    const int unexpected_events = poll_socket_events(fresh_fd, 450ms);
    expect_true(unexpected_events == 0,
                "fresh idle client should not receive stale response or be closed before sending request");

    const std::string fresh_request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fresh_fd, fresh_request), "fresh request should send");

    std::string carry;
    const std::string fresh_response = read_one_response(fresh_fd, carry);
    ::close(fresh_fd);

    expect_contains(fresh_response, "HTTP/1.1 200 OK", "fresh client should get 200 from healthz");
    expect_contains(fresh_response, R"({"status":"ok"})", "fresh client should get healthz body");
}

void test_concurrent_requests() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 4;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    constexpr int concurrency = 16;
    std::vector<std::future<bool>> futures;
    futures.reserve(concurrency);

    for (int idx = 0; idx < concurrency; ++idx) {
        futures.push_back(std::async(std::launch::async, [&server]() {
            const int fd = connect_localhost(server.listening_port());
            if (fd < 0) {
                return false;
            }

            const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            if (!send_all(fd, request)) {
                ::close(fd);
                return false;
            }

            std::string carry;
            const std::string response = read_one_response(fd, carry);
            ::close(fd);
            return response.find("HTTP/1.1 200 OK") != std::string::npos;
        }));
    }

    for (auto& future : futures) {
        expect_true(future.get(), "concurrent request should succeed");
    }
}

void test_concurrent_start_allows_only_one_success() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::promise<void> launch;
    const std::shared_future<void> go = launch.get_future().share();

    const auto start_once = [&server, go]() {
        go.wait();
        return server.start();
    };

    std::future<RunResult> first = std::async(std::launch::async, start_once);
    std::future<RunResult> second = std::async(std::launch::async, start_once);
    launch.set_value();

    for (int idx = 0; idx < 200; ++idx) {
        if (server.is_running()) {
            break;
        }
        const bool first_ready = first.wait_for(0ms) == std::future_status::ready;
        const bool second_ready = second.wait_for(0ms) == std::future_status::ready;
        if (first_ready && second_ready) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    server.stop();
    const RunResult first_result = first.get();
    const RunResult second_result = second.get();

    const int rejected_count = static_cast<int>(first_result == RunResult::StartRejected) +
                               static_cast<int>(second_result == RunResult::StartRejected);
    expect_true(rejected_count == 1, "concurrent start should reject exactly one start attempt");
    expect_true(is_nonfatal_run_result(first_result) || is_nonfatal_run_result(second_result),
                "accepted concurrent start should exit without fatal error");
    expect_true(!server.is_running(), "server should stop after stop request");
}

void test_early_stop_during_start_leaves_server_reusable() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::future<RunResult> start_future = std::async(std::launch::async, [&server]() { return server.start(); });

    bool start_finished = false;
    for (int idx = 0; idx < 300; ++idx) {
        server.stop();
        if (start_future.wait_for(10ms) == std::future_status::ready) {
            start_finished = true;
            break;
        }
    }
    expect_true(start_finished, "early stop should make start return in bounded time");

    const RunResult result = start_future.get();
    expect_true(is_nonfatal_run_result(result), "start should exit gracefully after early stop");
    expect_true(!server.is_running(), "server should not be running after early stop");
    expect_true(server.listening_port() == 0, "listening port should be cleared after early stop");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed after restart");

    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request after restart should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "server should serve healthz after restart");
}

void test_single_prestart_stop_cancels_start_once_and_server_remains_reusable() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    server.stop();
    std::future<RunResult> start_future = std::async(std::launch::async, [&server]() { return server.start(); });
    expect_true(start_future.wait_for(1s) == std::future_status::ready,
                "prestart stop should make start return quickly");
    const RunResult result = start_future.get();
    expect_true(result == RunResult::StartCanceled, "prestart stop should cancel start");
    expect_true(!server.is_running(), "server should stay stopped after prestart stop");
    expect_true(server.listening_port() == 0, "listening port should stay zero after prestart stop");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed after canceled start");
    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request after canceled start should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "server should be reusable after prestart stop cancellation");
}

void test_graceful_stop_completes_inflight_request() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);
    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow-graceful",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(250ms);
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow-graceful";
                                     return response;
                                 }),
                "add slow-graceful route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /slow-graceful HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    expect_true(send_all(fd, request), "slow request should send");

    std::string carry;
    std::future<std::string> response_future =
        std::async(std::launch::async, [fd, &carry]() { return read_one_response(fd, carry); });

    std::this_thread::sleep_for(30ms);
    server.stop();

    expect_true(response_future.wait_for(2s) == std::future_status::ready,
                "inflight request should complete before graceful stop");
    const std::string response = response_future.get();
    ::close(fd);

    expect_contains(response, "HTTP/1.1 200 OK", "graceful stop should preserve inflight response status");
    expect_contains(response, "slow-graceful", "graceful stop should preserve inflight response body");
}

void test_graceful_stop_rejects_new_connections_quickly() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);
    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow-stop-gate",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(300ms);
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow-stop-gate";
                                     return response;
                                 }),
                "add slow-stop-gate route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int inflight_fd = connect_localhost(server.listening_port());
    expect_true(inflight_fd >= 0, "connect should succeed");

    const std::string request = "GET /slow-stop-gate HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    expect_true(send_all(inflight_fd, request), "slow request should send");

    std::string carry;
    std::future<std::string> response_future =
        std::async(std::launch::async, [inflight_fd, &carry]() { return read_one_response(inflight_fd, carry); });

    std::this_thread::sleep_for(30ms);
    const auto stop_requested_at = std::chrono::steady_clock::now();
    server.stop();

    bool rejected = false;
    for (int idx = 0; idx < 80; ++idx) {
        const int probe_fd = connect_localhost(server.listening_port());
        if (probe_fd < 0) {
            rejected = true;
            break;
        }
        ::close(probe_fd);
        std::this_thread::sleep_for(10ms);
    }

    const auto reject_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_requested_at);
    expect_true(rejected, "new connection should be rejected after stop request");
    expect_true(reject_elapsed < 600ms, "new connection rejection should happen quickly after stop request");

    expect_true(response_future.wait_for(2s) == std::future_status::ready,
                "inflight request should still complete while shutdown gate is active");
    const std::string response = response_future.get();
    ::close(inflight_fd);
    expect_contains(response, "HTTP/1.1 200 OK", "inflight request should still receive a successful response");
}

void test_graceful_stop_timeout_forces_close_in_bounded_time() {
    using namespace std::chrono_literals;

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 1;
    config.graceful_shutdown_timeout = 250ms;
    nebula::server::HttpServer server(config);
    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow-stop-timeout",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(2s);
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow-stop-timeout";
                                     return response;
                                 }),
                "add slow-stop-timeout route should succeed");

    RunResult run_result = RunResult::FatalError;
    std::thread server_thread([&server, &run_result]() { run_result = server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");
    const std::string request = "GET /slow-stop-timeout HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    expect_true(send_all(fd, request), "slow timeout request should send");

    std::this_thread::sleep_for(60ms);
    const auto stop_started_at = std::chrono::steady_clock::now();
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    const auto stop_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_started_at);
    ::close(fd);

    expect_true(stop_elapsed >= 180ms, "graceful stop should wait near configured timeout before force close");
    expect_true(stop_elapsed < 1500ms, "graceful stop should force close in bounded time");
    expect_true(run_result == RunResult::ForcedByTimeout, "graceful stop timeout should report forced_by_timeout");
}

void test_add_route_while_running() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 4;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string path = "/dynamic-runtime";
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    const auto send_request = [&](const std::string& raw_request) {
        const int fd = connect_localhost(server.listening_port());
        expect_true(fd >= 0, "connect should succeed");
        expect_true(send_all(fd, raw_request), "send request should succeed");

        std::string carry;
        const std::string response = read_one_response(fd, carry);
        ::close(fd);
        return response;
    };

    const std::string before_response = send_request(request);
    expect_contains(before_response, "HTTP/1.1 404 Not Found", "new route should be 404 before add_route");

    expect_true(server.add_route(nebula::http::HttpMethod::Get, path,
                                 [](const nebula::http::HttpRequest&) {
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "dynamic route ready";
                                     return response;
                                 }),
                "first add route should succeed");
    expect_true(!server.add_route(nebula::http::HttpMethod::Get, path,
                                  [](const nebula::http::HttpRequest&) {
                                      nebula::http::HttpResponse response;
                                      response.status = nebula::http::HttpStatus::OK;
                                      response.headers.emplace("Content-Type", "text/plain");
                                      response.body = "duplicate";
                                      return response;
                                  }),
                "duplicate add route should fail");

    const std::string after_response = send_request(request);
    expect_contains(after_response, "HTTP/1.1 200 OK", "new route should be reachable after add_route");
    expect_contains(after_response, "dynamic route ready", "new route should return expected body");
}

void test_del_route_while_running() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 4;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const auto send_request = [&](const std::string& raw_request) {
        const int fd = connect_localhost(server.listening_port());
        expect_true(fd >= 0, "connect should succeed");
        expect_true(send_all(fd, raw_request), "send request should succeed");

        std::string carry;
        const std::string response = read_one_response(fd, carry);
        ::close(fd);
        return response;
    };

    const std::string before_response = send_request(request);
    expect_contains(before_response, "HTTP/1.1 200 OK", "healthz should be reachable before del_route");

    expect_true(server.del_route(nebula::http::HttpMethod::Get, "/healthz"), "del existing route should succeed");
    expect_true(!server.del_route(nebula::http::HttpMethod::Get, "/healthz"),
                "del same route twice should report false");

    const std::string after_response = send_request(request);
    expect_contains(after_response, "HTTP/1.1 404 Not Found", "healthz should be 404 after del_route");
}

void test_mod_route_while_running() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 4;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const auto send_request = [&](const std::string& raw_request) {
        const int fd = connect_localhost(server.listening_port());
        expect_true(fd >= 0, "connect should succeed");
        expect_true(send_all(fd, raw_request), "send request should succeed");

        std::string carry;
        const std::string response = read_one_response(fd, carry);
        ::close(fd);
        return response;
    };

    const std::string before_response = send_request(request);
    expect_contains(before_response, R"({"status":"ok"})", "healthz default body should exist before mod_route");

    expect_true(server.mod_route(nebula::http::HttpMethod::Get, "/healthz",
                                 [](const nebula::http::HttpRequest&) {
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "healthz modified";
                                     return response;
                                 }),
                "mod existing route should succeed");
    expect_true(!server.mod_route(nebula::http::HttpMethod::Get, "/missing",
                                  [](const nebula::http::HttpRequest&) {
                                      nebula::http::HttpResponse response;
                                      response.status = nebula::http::HttpStatus::OK;
                                      response.headers.emplace("Content-Type", "text/plain");
                                      response.body = "missing";
                                      return response;
                                  }),
                "mod missing route should fail");

    const std::string after_response = send_request(request);
    expect_contains(after_response, "HTTP/1.1 200 OK", "healthz should still return 200 after mod_route");
    expect_contains(after_response, "healthz modified", "healthz should return modified body");
}

void test_method_not_allowed_includes_allow_header() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/allow-check",
                                 [](const nebula::http::HttpRequest&) {
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "get";
                                     return response;
                                 }),
                "add get route should succeed");
    expect_true(server.add_route(nebula::http::HttpMethod::Post, "/allow-check",
                                 [](const nebula::http::HttpRequest&) {
                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "post";
                                     return response;
                                 }),
                "add post route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "PUT /allow-check HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 405 Method Not Allowed", "unknown route method should return 405");
    expect_contains(response, "\r\nAllow: GET, POST\r\n", "405 should include Allow header with method set");
    expect_contains(response, "Method Not Allowed", "error body should keep method not allowed text");
}

void test_chunked_request_rejected() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 501 Not Implemented", "chunked request should return 501");
    expect_contains(response, "Unsupported Transfer-Encoding", "error body should explain rejection");
}

void test_missing_host_returns_400() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/1.1\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 400 Bad Request", "missing host should return 400");
    expect_contains(response, "Missing Host Header", "error body should explain missing host");
}

void test_empty_host_returns_400() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/1.1\r\nHost:\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 400 Bad Request", "empty host should return 400");
    expect_contains(response, "Invalid Host Header", "error body should explain empty host rejection");
}

void test_unknown_method_returns_501() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "FOO /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 501 Not Implemented", "unknown method should return 501");
    expect_contains(response, "Unsupported HTTP Method", "error body should explain unsupported method");
}

void test_unsupported_http_version_returns_505() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/2\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 505 HTTP Version Not Supported", "unsupported version should return 505");
    expect_contains(response, "HTTP Version Not Supported", "error body should explain unsupported version");
}

void test_invalid_http_version_returns_400() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/2.beta\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 400 Bad Request", "invalid version format should return 400");
    expect_contains(response, "Invalid HTTP Version", "error body should explain invalid version format");
}

void test_content_too_large_returns_413() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    config.max_body_bytes = 8U;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string body = "0123456789";
    const std::string request = std::string("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ") +
                                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 413 Content Too Large", "oversized body should return 413");
    expect_contains(response, "Content Too Large", "error body should explain oversized body");
}

void test_header_too_large_returns_431() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    config.max_header_bytes = 128U;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Oversized: " + std::string(256U, 'a') +
                                "\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 431 Request Header Fields Too Large", "oversized header should return 431");
    expect_contains(response, "Request Header Fields Too Large", "error body should explain oversized header");
}

void test_uri_too_long_returns_414() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    config.max_request_target_bytes = 8U;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "GET /healthzz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    std::string carry;
    const std::string response = read_one_response(fd, carry);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 414 URI Too Long", "oversized request-target should return 414");
    expect_contains(response, "URI Too Long", "error body should explain oversized request-target");
}

void test_head_parse_error_suppresses_body() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string request = "HEAD /healthz HTTP/1.1\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, request), "send request should succeed");

    const std::string response = read_until_close(fd);
    ::close(fd);

    expect_contains(response, "HTTP/1.1 400 Bad Request", "head parse error should return 400");

    const std::size_t header_end = response.find("\r\n\r\n");
    expect_true(header_end != std::string::npos, "head parse error response should contain header terminator");

    const std::size_t content_length = parse_content_length(response.substr(0, header_end + 2U));
    expect_true(content_length > 0U, "head parse error should keep non-zero content-length");

    const std::size_t actual_body_len = response.size() - (header_end + 4U);
    expect_true(actual_body_len == 0U, "head parse error response must not include body bytes");
}

void test_parse_error_responds_once_before_close() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    nebula::server::HttpServer server(config);

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string bad_request =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    expect_true(send_all(fd, bad_request), "send bad request should succeed");

    const std::string extra = std::string(512U, 'x');
    for (int idx = 0; idx < 64; ++idx) {
        if (!send_all(fd, extra)) {
            break;
        }
    }

    const std::string all_responses = read_until_close(fd);
    ::close(fd);

    expect_contains(all_responses, "HTTP/1.1 501 Not Implemented", "parse error should return 501");
    const std::size_t response_count = count_occurrences(all_responses, "HTTP/1.1 ");
    expect_true(response_count == 1U, "parse error should respond once before closing connection");
}

void test_processing_state_pending_buffer_limit() {
    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_threads = 2;
    config.max_header_bytes = 96U;
    config.max_body_bytes = 32U;
    nebula::server::HttpServer server(config);
    expect_true(server.add_route(nebula::http::HttpMethod::Get, "/slow",
                                 [](const nebula::http::HttpRequest&) {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(250));

                                     nebula::http::HttpResponse response;
                                     response.status = nebula::http::HttpStatus::OK;
                                     response.headers.emplace("Content-Type", "text/plain");
                                     response.body = "slow";
                                     return response;
                                 }),
                "add slow route should succeed");

    std::thread server_thread([&server]() { server.start(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const int fd = connect_localhost(server.listening_port());
    expect_true(fd >= 0, "connect should succeed");

    const std::string slow_request = "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    expect_true(send_all(fd, slow_request), "slow request should send");

    const std::string queued_close_request = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (int idx = 0; idx < 32; ++idx) {
        if (!send_all(fd, queued_close_request)) {
            break;
        }
    }

    const std::string all_responses = read_until_close(fd);
    ::close(fd);

    const std::size_t response_count = count_occurrences(all_responses, "HTTP/1.1 ");
    expect_true(response_count <= 1U, "pending buffer limit should close connection before multiple queued responses");
}

int run_http_server_integration_tests() {
    const nebula::testsupport::TempDir log_dir("nebula-http-server-integration-log");
    nebula::common::Logger::instance().init(nebula::common::LogLevel::Warning, log_dir.path(), false);

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"signal mask restored after http server destroyed", test_signal_mask_restored_after_http_server_destroyed},
        {"signal shutdown remains available after restart", test_signal_shutdown_remains_available_after_restart},
        {"signal shutdown with preexisting unmasked thread", test_signal_shutdown_with_preexisting_unmasked_thread},
        {"healthz endpoint", test_healthz_endpoint},
        {"healthz endpoint absolute form", test_healthz_endpoint_absolute_form},
        {"healthz endpoint with query", test_healthz_endpoint_with_query},
        {"root redirects to healthz", test_root_redirects_to_healthz},
        {"head method suppresses body", test_head_method_suppresses_body},
        {"echo endpoint", test_echo_endpoint},
        {"request completed log uses raw request line", test_request_completed_log_uses_raw_request_line},
        {"keep alive two requests", test_keep_alive_two_requests},
        {"client reset during response keeps server running", test_client_reset_during_response_keeps_server_running},
        {"stale async response not delivered to new connection",
         test_stale_async_response_not_delivered_to_new_connection},
        {"concurrent requests", test_concurrent_requests},
        {"concurrent start allows only one success", test_concurrent_start_allows_only_one_success},
        {"early stop during start leaves server reusable", test_early_stop_during_start_leaves_server_reusable},
        {"single prestart stop cancels start once and server remains reusable",
         test_single_prestart_stop_cancels_start_once_and_server_remains_reusable},
        {"graceful stop completes inflight request", test_graceful_stop_completes_inflight_request},
        {"graceful stop rejects new connections quickly", test_graceful_stop_rejects_new_connections_quickly},
        {"graceful stop timeout forces close in bounded time", test_graceful_stop_timeout_forces_close_in_bounded_time},
        {"add route while running", test_add_route_while_running},
        {"del route while running", test_del_route_while_running},
        {"mod route while running", test_mod_route_while_running},
        {"method not allowed includes allow header", test_method_not_allowed_includes_allow_header},
        {"chunked request rejected", test_chunked_request_rejected},
        {"missing host returns 400", test_missing_host_returns_400},
        {"empty host returns 400", test_empty_host_returns_400},
        {"unknown method returns 501", test_unknown_method_returns_501},
        {"unsupported http version returns 505", test_unsupported_http_version_returns_505},
        {"invalid http version returns 400", test_invalid_http_version_returns_400},
        {"content too large returns 413", test_content_too_large_returns_413},
        {"header too large returns 431", test_header_too_large_returns_431},
        {"uri too long returns 414", test_uri_too_long_returns_414},
        {"head parse error suppresses body", test_head_parse_error_suppresses_body},
        {"parse error responds once before close", test_parse_error_responds_once_before_close},
        {"processing state pending buffer limit", test_processing_state_pending_buffer_limit},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_http_server_integration_tests);
}
