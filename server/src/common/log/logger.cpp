#include "nebula/common/log/logger.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "nebula/common/log/types.hpp"
#include "nebula/common/platform/posix.hpp"

namespace nebula::common {

namespace {

struct TimeText {
    std::string date;
    std::string timestamp;
};

template <typename TimePoint>
TimeText format_time_text(TimePoint now) {
    const auto now_ms = std::chrono::floor<std::chrono::milliseconds>(now);
    return {.date = std::format("{:%F}", now_ms), .timestamp = std::format("{:%FT%T}", now_ms)};
}

std::string format_utc_offset(std::chrono::seconds offset) {
    bool negative = offset < std::chrono::seconds{0};
    if (negative) {
        offset = -offset;
    }

    const std::int64_t total = offset.count();
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t seconds = total % 60;

    if (seconds == 0) {
        return std::format("{}{:02d}:{:02d}", negative ? "-" : "+", hours, minutes);
    }
    return std::format("{}{:02d}:{:02d}:{:02d}", negative ? "-" : "+", hours, minutes, seconds);
}

TimeText format_local_time_text(std::chrono::system_clock::time_point now) {
    try {
        const std::chrono::time_zone* zone = std::chrono::current_zone();
        const std::chrono::zoned_time zoned_now{zone, now};
        TimeText time_text = format_time_text(zoned_now.get_local_time());
        time_text.timestamp.append(
            format_utc_offset(std::chrono::duration_cast<std::chrono::seconds>(zone->get_info(now).offset)));
        return time_text;
    } catch (const std::runtime_error&) {
        TimeText time_text = format_time_text(now);
        time_text.timestamp.push_back('Z');
        return time_text;
    }
}

std::string format_double_field_value(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-inf" : "inf";
    }

    std::string dumped = std::format("{:.17g}", value);
    if (dumped.find_first_of(".eE") == std::string::npos) {
        dumped.append(".0");
    }
    return dumped;
}

std::string escape_log_string(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 2U);
    output.push_back('"');

    for (const unsigned char ch : text) {
        switch (ch) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\b':
                output.append("\\b");
                break;
            case '\f':
                output.append("\\f");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                if (std::iscntrl(ch) != 0) {
                    output.append(std::format("\\u{:04x}", ch));
                } else {
                    output.push_back(static_cast<char>(ch));
                }
                break;
        }
    }

    output.push_back('"');
    return output;
}

std::string format_field_value(const FieldValue& value) {
    return std::visit(
        [](const auto& typed_value) -> std::string {
            using Type = std::remove_cvref_t<decltype(typed_value)>;
            if constexpr (std::same_as<Type, std::string>) {
                return escape_log_string(typed_value);
            } else if constexpr (std::same_as<Type, std::nullptr_t>) {
                return "null";
            } else if constexpr (std::same_as<Type, bool>) {
                return typed_value ? "true" : "false";
            } else if constexpr (std::same_as<Type, std::int64_t>) {
                return std::format("{}", typed_value);
            } else {
                return format_double_field_value(typed_value);
            }
        },
        value);
}

std::string format_log_line(std::string_view timestamp, LogLevel level, std::string_view event,
                            std::span<const Field> fields) {
    std::string line = std::format("[{}] [{}] {}", timestamp, to_string(level), event);
    if (fields.empty()) {
        return line;
    }

    line.append(": ");
    for (std::size_t idx = 0; idx < fields.size(); ++idx) {
        if (idx > 0U) {
            line.append(", ");
        }
        line.append(fields[idx].key);
        line.push_back('=');
        line.append(format_field_value(fields[idx].value));
    }
    return line;
}

void write_direct_stderr_log(LogLevel level, std::string_view event, std::span<const Field> fields) noexcept {
    try {
        const TimeText time_text = format_local_time_text(std::chrono::system_clock::now());
        std::cerr << format_log_line(time_text.timestamp, level, event, fields) << '\n';
    } catch (...) {
        std::fputs("logger write failed\n", stderr);
    }
}

void report_entry_emit_error(const char* error) noexcept {
    const std::array<Field, 1> fields = {Field("error", error != nullptr ? error : "unknown")};
    write_direct_stderr_log(LogLevel::Error, "logger entry emit failed", fields);
}

void report_entry_field_append_error(const char* error) noexcept {
    const std::array<Field, 1> fields = {Field("error", error != nullptr ? error : "unknown")};
    write_direct_stderr_log(LogLevel::Error, "logger entry field append failed", fields);
}

void report_logger_api_error(const char* api, const char* error) noexcept {
    const std::array<Field, 2> fields = {Field("api", api != nullptr ? api : "unknown"),
                                         Field("error", error != nullptr ? error : "unknown")};
    write_direct_stderr_log(LogLevel::Error, "logger api failed", fields);
}

void report_logger_internal_error(const char* stage, const char* error) noexcept {
    const std::array<Field, 2> fields = {Field("stage", stage != nullptr ? stage : "unknown"),
                                         Field("error", error != nullptr ? error : "unknown")};
    write_direct_stderr_log(LogLevel::Error, "logger internal failed", fields);
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Entry::Entry(Logger& logger, LogLevel level, std::string_view event)
    : logger_(&logger), level_(level), event_(event) {}

Logger::Entry::Entry(Entry&& other) noexcept
    : logger_(other.logger_),
      level_(other.level_),
      event_(std::move(other.event_)),
      fields_(std::move(other.fields_)),
      emitted_(other.emitted_) {
    other.logger_ = nullptr;
    other.emitted_ = true;
}

void Logger::Entry::report_field_append_error(const char* error) noexcept {
    report_entry_field_append_error(error);
}

Logger::Entry::~Entry() noexcept {
    emit();
}

Logger::Entry& Logger::Entry::field(Field field_value) noexcept {
    try {
        fields_.push_back(std::move(field_value));
    } catch (const std::exception& e) {
        report_field_append_error(e.what());
    } catch (...) {
        report_field_append_error("unknown");
    }
    return *this;
}

void Logger::Entry::emit() noexcept {
    if (logger_ == nullptr || emitted_) {
        return;
    }

    try {
        emitted_ = true;
        logger_->log(level_, event_, fields_);
    } catch (const std::exception& e) {
        report_entry_emit_error(e.what());
    } catch (...) {
        report_entry_emit_error("unknown");
    }
}

Logger::Entry Logger::trace(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Trace, event};
    } catch (const std::exception& e) {
        report_logger_api_error("trace", e.what());
    } catch (...) {
        report_logger_api_error("trace", "unknown");
    }
    return {};
}

Logger::Entry Logger::debug(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Debug, event};
    } catch (const std::exception& e) {
        report_logger_api_error("debug", e.what());
    } catch (...) {
        report_logger_api_error("debug", "unknown");
    }
    return {};
}

Logger::Entry Logger::info(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Info, event};
    } catch (const std::exception& e) {
        report_logger_api_error("info", e.what());
    } catch (...) {
        report_logger_api_error("info", "unknown");
    }
    return {};
}

Logger::Entry Logger::warn(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Warning, event};
    } catch (const std::exception& e) {
        report_logger_api_error("warn", e.what());
    } catch (...) {
        report_logger_api_error("warn", "unknown");
    }
    return {};
}

Logger::Entry Logger::error(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Error, event};
    } catch (const std::exception& e) {
        report_logger_api_error("error", e.what());
    } catch (...) {
        report_logger_api_error("error", "unknown");
    }
    return {};
}

Logger::Entry Logger::fatal(std::string_view event) noexcept {
    try {
        return {*this, LogLevel::Fatal, event};
    } catch (const std::exception& e) {
        report_logger_api_error("fatal", e.what());
    } catch (...) {
        report_logger_api_error("fatal", "unknown");
    }
    return {};
}

void Logger::initialize(LoggerConfig config) noexcept {
    try {
        std::lock_guard lock(mutex_);
        level_ = config.level;
        log_dir_ = std::move(config.dir);
        stderr_enabled_ = config.also_stderr;
        file_enabled_ = true;

        current_date_.clear();
        file_.close();
        file_.clear();

        const auto now = std::chrono::system_clock::now();
        const TimeText time_text = format_local_time_text(now);
        rotate_file_if_needed_locked(time_text.date, time_text.timestamp);
    } catch (const std::exception& e) {
        report_logger_api_error("initialize", e.what());
    } catch (...) {
        report_logger_api_error("initialize", "unknown");
    }
}

void Logger::set_level(LogLevel level) noexcept {
    try {
        std::lock_guard lock(mutex_);
        level_ = level;
    } catch (const std::exception& e) {
        report_logger_api_error("set_level", e.what());
    } catch (...) {
        report_logger_api_error("set_level", "unknown");
    }
}

LogLevel Logger::level() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return level_;
    } catch (const std::exception& e) {
        report_logger_api_error("level", e.what());
    } catch (...) {
        report_logger_api_error("level", "unknown");
    }
    return LogLevel::Info;
}

void Logger::log(LogLevel level, std::string_view event, std::span<const Field> fields) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!should_log(level)) {
            return;
        }

        const auto now = std::chrono::system_clock::now();
        const TimeText time_text = format_local_time_text(now);
        const std::string line = format_log_line(time_text.timestamp, level, event, fields);
        rotate_file_if_needed_locked(time_text.date, time_text.timestamp);

        write_line_locked(line);
    } catch (const std::exception& e) {
        report_logger_internal_error("log", e.what());
    } catch (...) {
        report_logger_internal_error("log", "unknown");
    }
}

void Logger::rotate_file_if_needed_locked(std::string_view date, std::string_view timestamp) {
    if (!file_enabled_) {
        return;
    }

    if (current_date_ == date && file_.is_open()) {
        return;
    }

    std::error_code create_dir_error;
    std::filesystem::create_directories(log_dir_, create_dir_error);
    if (create_dir_error) {
        const std::array<Field, 4> fields = {
            Field("path", log_dir_),
            Field("errno", create_dir_error.value()),
            Field("error", create_dir_error.message()),
            Field("fallback", "stderr_only"),
        };
        std::cerr << format_log_line(timestamp, LogLevel::Error, "create log directory failed", fields) << '\n';
        file_.close();
        current_date_ = date;
        file_enabled_ = false;
        stderr_enabled_ = true;
        return;
    }

    file_.close();
    file_.clear();
    const auto file_path = log_dir_ / std::format("nebula-{}.log", date);
    errno = 0;
    file_.open(file_path, std::ios::app);
    if (!file_.is_open()) {
        const int err = errno;
        const std::array<Field, 4> fields = {
            Field("path", file_path),
            Field("errno", err),
            Field("error", common::errno_message(err)),
            Field("fallback", "stderr_only"),
        };
        std::cerr << format_log_line(timestamp, LogLevel::Error, "open log file failed", fields) << '\n';
        current_date_ = date;
        file_enabled_ = false;
        stderr_enabled_ = true;
        return;
    }

    current_date_ = date;
}

void Logger::write_line_locked(std::string_view line) {
    if (stderr_enabled_) {
        std::cerr << line << '\n';
    }

    if (file_enabled_ && file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
}

bool Logger::should_log(LogLevel level) const {
    return level >= level_;
}

}  // namespace nebula::common
