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

}  // namespace

std::string_view to_string(LogLevel level) {
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
    }
    return "UNKNOWN";
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

Logger::Entry::~Entry() {
    try {
        emit();
    } catch (...) {
        std::fputs("logger entry emit failed: error=unknown, decision=ignore\n", stderr);
    }
}

Logger::Entry& Logger::Entry::field(Field field_value) {
    fields_.push_back(std::move(field_value));
    return *this;
}

void Logger::Entry::emit() {
    if (logger_ == nullptr || emitted_) {
        return;
    }

    emitted_ = true;
    logger_->log(level_, event_, fields_);
}

Logger::Entry Logger::trace(std::string_view event) {
    return {*this, LogLevel::Trace, event};
}

Logger::Entry Logger::debug(std::string_view event) {
    return {*this, LogLevel::Debug, event};
}

Logger::Entry Logger::info(std::string_view event) {
    return {*this, LogLevel::Info, event};
}

Logger::Entry Logger::warn(std::string_view event) {
    return {*this, LogLevel::Warning, event};
}

Logger::Entry Logger::error(std::string_view event) {
    return {*this, LogLevel::Error, event};
}

void Logger::init(LogLevel default_level, std::filesystem::path log_dir, bool also_stderr) {
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
    rotate_file_if_needed_unlocked(time_text.date, time_text.timestamp);
}

void Logger::set_level(LogLevel level) {
    std::lock_guard lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard lock(mutex_);
    return level_;
}

void Logger::log(LogLevel level, std::string_view event, std::span<const Field> fields) {
    std::lock_guard lock(mutex_);
    ensure_initialized_unlocked();
    if (!should_log(level)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const TimeText time_text = format_local_time_text(now);
    rotate_file_if_needed_unlocked(time_text.date, time_text.timestamp);

    const std::string line = format_log_line(time_text.timestamp, level, event, fields);
    write_line_unlocked(line);
}

void Logger::ensure_initialized_unlocked() {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    const auto now = std::chrono::system_clock::now();
    const TimeText time_text = format_local_time_text(now);
    rotate_file_if_needed_unlocked(time_text.date, time_text.timestamp);

    const std::array<Field, 3> fields = {
        Field("log_dir", log_dir_.string()),
        Field("fallback", "default_config"),
        Field("decision", "continue_with_default_config"),
    };
    const std::string line = format_log_line(time_text.timestamp, LogLevel::Warning, "logger auto initialized", fields);
    write_line_unlocked(line);
}

void Logger::rotate_file_if_needed_unlocked(std::string_view date, std::string_view timestamp) {
    if (current_date_ == date && file_.is_open()) {
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
        current_date_.clear();
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
        const std::string err_text = err == 0 ? "unknown" : std::system_category().message(err);
        const std::array<Field, 3> fields = {
            Field("path", file_path.string()),
            Field("errno", err, err_text),
            Field("fallback", "stderr_only"),
        };
        std::cerr << format_log_line(timestamp, LogLevel::Error, "open log file failed", fields) << '\n';
        current_date_.clear();
        force_stderr_only_ = true;
        return;
    }

    current_date_ = date;
    force_stderr_only_ = false;
}

void Logger::write_line_unlocked(std::string_view line) {
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
