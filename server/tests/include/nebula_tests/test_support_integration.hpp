#ifndef NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP
#define NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP

#include <cerrno>
#include <cstdint>
#include <cstdlib>
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

#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula/server/server_builder.hpp"
#include "nebula_tests/test_support.hpp"

namespace nebula::testsupport::integration {

namespace detail {

inline sockaddr* as_sockaddr(sockaddr_in& addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&addr);
}

}  // namespace detail

inline constexpr std::string_view kIntegrationJwtSecret = "integration_auth_secret_0123456789abcdef";

inline void apply_database_config(nebula::app::ServerConfig& config,
                                  const nebula::common::PostgresConnectionPoolOptions& db_config) {
    config.database_host = db_config.host;
    config.database_port = db_config.port;
    config.database_name = db_config.database;
    config.database_user = db_config.user;
    ::setenv("NEBULA_TEST_DATABASE_PASSWORD_RUNTIME", db_config.password.c_str(), 1);
    config.database_password_env = "NEBULA_TEST_DATABASE_PASSWORD_RUNTIME";
}

inline nebula::server::ServerRuntime build_runtime(nebula::app::ServerConfig config,
                                                   std::shared_ptr<nebula::http::Router> router,
                                                   std::shared_ptr<nebula::auth::AuthService> auth_service = nullptr) {
    return nebula::server::ServerBuilder()
        .with_config(std::move(config))
        .with_router(std::move(router))
        .with_auth_service(std::move(auth_service))
        .build();
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

inline void wait_until_server_ready(nebula::server::ServerRuntime& server) {
    using namespace std::chrono_literals;

    for (int idx = 0; idx < 200; ++idx) {
        if (server.is_running() && server.listening_port() > 0) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }

    fail("server did not become ready in time");
}

class ServerThreadGuard {
public:
    ServerThreadGuard(nebula::server::ServerRuntime& server, std::thread& thread) : server_(server), thread_(thread) {}

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
    nebula::server::ServerRuntime& server_;
    std::thread& thread_;
};

}  // namespace nebula::testsupport::integration

#endif  // NEBULA_TESTS_TEST_SUPPORT_INTEGRATION_HPP
