#ifndef NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP
#define NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nebula/server/http_server.hpp"
#include "nebula_tests/test_support.hpp"

namespace nebula::testsupport::integration {

namespace detail {

inline sockaddr* as_sockaddr(sockaddr_in& addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&addr);
}

}  // namespace detail

inline nebula::server::HttpServerRuntime build_runtime(nebula::server::ServerConfig config,
                                                       std::shared_ptr<nebula::http::Router> router) {
    return nebula::server::HttpServerBuilder().with_config(std::move(config)).with_router(std::move(router)).build();
}

inline bool send_all(int fd, std::string_view data) {
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

inline int connect_localhost(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, detail::as_sockaddr(addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

inline std::string read_until_close(int fd) {
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

inline void wait_until_server_ready(nebula::server::HttpServerRuntime& server) {
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
    ServerThreadGuard(nebula::server::HttpServerRuntime& server, std::thread& thread)
        : server_(server), thread_(thread) {}

    ServerThreadGuard(const ServerThreadGuard&) = delete;
    ServerThreadGuard& operator=(const ServerThreadGuard&) = delete;
    ServerThreadGuard(ServerThreadGuard&&) = delete;
    ServerThreadGuard& operator=(ServerThreadGuard&&) = delete;

    ~ServerThreadGuard() noexcept {
        server_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    nebula::server::HttpServerRuntime& server_;
    std::thread& thread_;
};

}  // namespace nebula::testsupport::integration

#endif  // NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP
