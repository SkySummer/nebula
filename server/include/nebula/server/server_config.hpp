#ifndef NEBULA_SERVER_SERVER_CONFIG_HPP
#define NEBULA_SERVER_SERVER_CONFIG_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "nebula/common/logger.hpp"

namespace nebula::server {

std::size_t default_worker_thread_count();
std::size_t default_sub_reactor_count();

struct ServerConfig {
    std::uint16_t port = 8080;
    int backlog = 1024;
    std::size_t max_connections = 4096;
    std::size_t sub_reactor_count = default_sub_reactor_count();
    std::size_t worker_thread_count = default_worker_thread_count();
    bool manage_signals = true;

    common::LogLevel log_level = common::LogLevel::Info;
    std::filesystem::path log_dir = "runtime/logs";
    bool log_also_stderr = true;

    std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds graceful_shutdown_timeout = std::chrono::seconds(5);

    std::size_t max_header_bytes = static_cast<std::size_t>(16U) * 1024U;
    std::size_t max_request_target_bytes = static_cast<std::size_t>(8U) * 1024U;
    std::size_t max_body_bytes = static_cast<std::size_t>(1024U) * 1024U;

    bool enable_healthz = true;
    bool enable_echo = true;
    bool enable_root_default = true;
    std::string root_default_path = "/healthz";
};

void normalize_server_thread_counts(ServerConfig& config);

enum class ServerConfigSource : std::uint8_t {
    Default,
    File,
};

[[nodiscard]] std::string_view to_string(ServerConfigSource source);

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
