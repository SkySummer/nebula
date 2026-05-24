#ifndef NEBULA_SERVER_CONFIG_CONFIG_HPP
#define NEBULA_SERVER_CONFIG_CONFIG_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace nebula::server {

inline constexpr std::size_t kMaxServerMaxConnections = 1'000'000U;
inline constexpr std::size_t kMaxServerSubReactorCount = 64U;
inline constexpr std::size_t kMaxServerWorkerThreadCount = 256U;

inline std::size_t default_sub_reactor_count() {
    const std::size_t hardware_count = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware_count / 2U, 1U, 8U);
}

inline std::size_t default_worker_thread_count() {
    const std::size_t hardware_count = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware_count, 2U, 16U);
}

struct ServerConfig {
    std::uint16_t port = 8080;
    int backlog = 1024;
    std::size_t max_connections = 4096;
    std::size_t sub_reactor_count = default_sub_reactor_count();
    std::size_t worker_thread_count = default_worker_thread_count();
    bool manage_signals = true;

    ServerConfig& normalize() &;
    ServerConfig&& normalize() &&;

    [[nodiscard]] bool validate() const;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_CONFIG_CONFIG_HPP
