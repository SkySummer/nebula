#ifndef NEBULA_SERVER_CONFIG_TIMEOUTS_CONFIG_HPP
#define NEBULA_SERVER_CONFIG_TIMEOUTS_CONFIG_HPP

#include <chrono>

namespace nebula::server {

struct ServerTimeoutConfig {
    std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds graceful_shutdown_timeout = std::chrono::seconds(5);
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_CONFIG_TIMEOUTS_CONFIG_HPP
