#ifndef NEBULA_COMMON_LOG_TYPES_HPP
#define NEBULA_COMMON_LOG_TYPES_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace nebula::common {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

struct LoggerConfig {
    LogLevel level = LogLevel::Info;
    std::filesystem::path dir = "runtime/logs";
    bool also_stderr = true;
};

}  // namespace nebula::common

#endif  // NEBULA_COMMON_LOG_TYPES_HPP
