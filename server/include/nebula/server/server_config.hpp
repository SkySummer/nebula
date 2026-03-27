#ifndef NEBULA_SERVER_SERVER_CONFIG_HPP
#define NEBULA_SERVER_SERVER_CONFIG_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include "nebula/common/logger.hpp"

namespace nebula::server {

inline std::size_t default_worker_threads() {
    const std::size_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0U) {
        return 2U;
    }
    return std::max<std::size_t>(2U, hardware / 2U);
}

struct ServerConfig {
    std::uint16_t port = 8080;
    int backlog = 1024;
    std::size_t max_connections = 4096;
    std::size_t worker_threads = default_worker_threads();
    bool manage_signals = true;

    common::LogLevel log_level = common::LogLevel::Info;
    std::filesystem::path log_dir = "runtime/logs";
    bool log_also_stderr = true;

    std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds graceful_shutdown_timeout = std::chrono::seconds(5);

    std::size_t max_header_bytes = static_cast<std::size_t>(16U) * 1024U;
    std::size_t max_request_target_bytes = static_cast<std::size_t>(8U) * 1024U;
    std::size_t max_body_bytes = static_cast<std::size_t>(1024U) * 1024U;
};

enum class ServerConfigSource : std::uint8_t {
    Default,
    File,
};

std::string_view to_string(ServerConfigSource source);

struct ServerConfigLoadResult {
    bool ok = true;
    ServerConfig config;
    ServerConfigSource source = ServerConfigSource::Default;
    std::size_t error_line = 0;
    std::string error;

    ServerConfigLoadResult() = default;
    explicit ServerConfigLoadResult(const std::filesystem::path& path);
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SERVER_CONFIG_HPP
