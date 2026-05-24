#include "nebula/common/log/types.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace nebula::common {

namespace {

std::string to_lower(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    std::unreachable();
}

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
    std::string lowered = to_lower(text);

    if (lowered == "trace") {
        return LogLevel::Trace;
    }
    if (lowered == "debug") {
        return LogLevel::Debug;
    }
    if (lowered == "info") {
        return LogLevel::Info;
    }
    if (lowered == "warn" || lowered == "warning") {
        return LogLevel::Warning;
    }
    if (lowered == "error") {
        return LogLevel::Error;
    }
    if (lowered == "fatal") {
        return LogLevel::Fatal;
    }
    return std::nullopt;
}

}  // namespace nebula::common
