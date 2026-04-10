#include "nebula/common/logger.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "nebula/common/posix_utils.hpp"

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

void append_field_text(std::string& out, const Field& field) {
    out.append(field.key);
    out.push_back('=');
    out.append(field.value);
    if (!field.message.empty()) {
        out.append(" (");
        out.append(field.message);
        out.push_back(')');
    }
}

void append_fields_text(std::string& out, std::span<const Field> fields) {
    bool first = true;
    for (const Field& field : fields) {
        if (!first) {
            out.append(", ");
        }
        first = false;

        append_field_text(out, field);
    }
}

std::string format_body(std::string_view event, std::span<const Field> fields) {
    std::string body(event);

    if (!fields.empty()) {
        body.append(": ");
        append_fields_text(body, fields);
    }

    return body;
}

std::string format_log_line(std::string_view timestamp, LogLevel level, std::string_view event,
                            std::span<const Field> fields) {
    return std::format("[{}] [{}] {}", timestamp, to_string(level), format_body(event, fields));
}

void report_entry_emit_error(const char* error) noexcept {
    std::fputs("logger entry emit failed: error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputc('\n', stderr);
}

void report_entry_field_append_error(const char* error) noexcept {
    std::fputs("logger entry field append failed: error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputc('\n', stderr);
}

void report_logger_api_error(const char* api, const char* error) noexcept {
    std::fputs("logger api failed: api=", stderr);
    std::fputs(api != nullptr ? api : "unknown", stderr);
    std::fputs(", error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputc('\n', stderr);
}

void report_logger_internal_error(const char* stage, const char* error) noexcept {
    std::fputs("logger internal failed: stage=", stderr);
    std::fputs(stage != nullptr ? stage : "unknown", stderr);
    std::fputs(", error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputc('\n', stderr);
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
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    return "UNKNOWN";
}

std::string_view to_string(LogDomain domain) noexcept {
    switch (domain) {
        case LogDomain::Auth:
            return "auth";
        case LogDomain::Common:
            return "common";
        case LogDomain::Http:
            return "http";
        case LogDomain::Server:
            return "server";
        case LogDomain::User:
            return "user";
        case LogDomain::Test:
            return "test";
    }
    return "unknown";
}

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

Logger::Entry Logger::trace(LogDomain domain, std::string_view event) noexcept {
    Entry entry = trace(event);
    entry.field("domain", to_string(domain));
    return entry;
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

Logger::Entry Logger::debug(LogDomain domain, std::string_view event) noexcept {
    Entry entry = debug(event);
    entry.field("domain", to_string(domain));
    return entry;
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

Logger::Entry Logger::info(LogDomain domain, std::string_view event) noexcept {
    Entry entry = info(event);
    entry.field("domain", to_string(domain));
    return entry;
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

Logger::Entry Logger::warn(LogDomain domain, std::string_view event) noexcept {
    Entry entry = warn(event);
    entry.field("domain", to_string(domain));
    return entry;
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

Logger::Entry Logger::error(LogDomain domain, std::string_view event) noexcept {
    Entry entry = error(event);
    entry.field("domain", to_string(domain));
    return entry;
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

Logger::Entry Logger::fatal(LogDomain domain, std::string_view event) noexcept {
    Entry entry = fatal(event);
    entry.field("domain", to_string(domain));
    return entry;
}

void Logger::initialize(LogLevel default_level, std::filesystem::path log_dir, bool also_stderr) noexcept {
    try {
        std::lock_guard lock(mutex_);
        level_ = default_level;
        log_dir_ = std::move(log_dir);
        also_stderr_ = also_stderr;
        force_stderr_only_ = false;
        initialized_ = true;

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
        ensure_initialized_locked();
        if (!should_log(level)) {
            return;
        }

        const auto now = std::chrono::system_clock::now();
        const TimeText time_text = format_local_time_text(now);
        rotate_file_if_needed_locked(time_text.date, time_text.timestamp);

        const std::string line = format_log_line(time_text.timestamp, level, event, fields);
        write_line_locked(line);
    } catch (const std::exception& e) {
        report_logger_internal_error("log", e.what());
    } catch (...) {
        report_logger_internal_error("log", "unknown");
    }
}

void Logger::ensure_initialized_locked() {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    const auto now = std::chrono::system_clock::now();
    const TimeText time_text = format_local_time_text(now);
    rotate_file_if_needed_locked(time_text.date, time_text.timestamp);

    const std::array<Field, 3> fields = {
        Field("log_dir", log_dir_.string()),
        Field("fallback", "default_config"),
        Field("decision", "continue_with_default_config"),
    };
    const std::string line = format_log_line(time_text.timestamp, LogLevel::Warning, "logger auto initialized", fields);
    write_line_locked(line);
}

void Logger::rotate_file_if_needed_locked(std::string_view date, std::string_view timestamp) {
    if (current_date_ == date && (file_.is_open() || force_stderr_only_)) {
        return;
    }

    std::error_code create_dir_error;
    std::filesystem::create_directories(log_dir_, create_dir_error);
    if (create_dir_error) {
        const std::array<Field, 3> fields = {
            Field("path", log_dir_.string()),
            Field("errno", create_dir_error.value(), create_dir_error.message()),
            Field("fallback", "stderr_only"),
        };
        std::cerr << format_log_line(timestamp, LogLevel::Error, "create log directory failed", fields) << '\n';
        file_.close();
        current_date_ = date;
        force_stderr_only_ = true;
        return;
    }

    file_.close();
    file_.clear();
    const auto file_path = log_dir_ / std::format("nebula-{}.log", date);
    errno = 0;
    file_.open(file_path, std::ios::app);
    if (!file_.is_open()) {
        const int err = errno;
        const std::array<Field, 3> fields = {
            Field("path", file_path.string()),
            Field("errno", err, common::errno_message(err)),
            Field("fallback", "stderr_only"),
        };
        std::cerr << format_log_line(timestamp, LogLevel::Error, "open log file failed", fields) << '\n';
        current_date_ = date;
        force_stderr_only_ = true;
        return;
    }

    current_date_ = date;
    force_stderr_only_ = false;
}

void Logger::write_line_locked(std::string_view line) {
    if (also_stderr_ || force_stderr_only_) {
        std::cerr << line << '\n';
    }

    if (!file_.is_open()) {
        return;
    }

    file_ << line << '\n';
    file_.flush();
}

bool Logger::should_log(LogLevel level) const {
    return level >= level_;
}

}  // namespace nebula::common
