#ifndef NEBULA_COMMON_LOGGER_HPP
#define NEBULA_COMMON_LOGGER_HPP

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nebula::common {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

[[nodiscard]] std::string_view to_string(LogLevel level);

struct Field {
    Field(std::string key_in, std::string value_in, std::string message_in = {})
        : key(std::move(key_in)), value(std::move(value_in)), message(std::move(message_in)) {}

    Field(std::string key_in, std::string_view value_in, std::string_view message_in = {})
        : key(std::move(key_in)), value(value_in), message(message_in) {}

    Field(std::string key_in, const char* value_in, std::string_view message_in = {})
        : key(std::move(key_in)), value(value_in == nullptr ? "null" : value_in), message(message_in) {}

    Field(std::string key_in, bool value_in, std::string_view message_in = {})
        : key(std::move(key_in)), value(value_in ? "true" : "false"), message(message_in) {}

    template <typename T>
        requires(requires(T&& v) { std::format("{}", std::forward<T>(v)); } &&
                 !std::same_as<std::remove_cvref_t<T>, std::string> &&
                 !std::same_as<std::remove_cvref_t<T>, std::string_view> &&
                 !std::same_as<std::remove_cvref_t<T>, const char*> && !std::same_as<std::remove_cvref_t<T>, char*>)
    Field(std::string key_in, T&& value_in, std::string_view message_in = {})
        : key(std::move(key_in)), value(std::format("{}", std::forward<T>(value_in))), message(message_in) {}

    std::string key;
    std::string value;
    std::string message;
};

class Logger {
public:
    class Entry {
    public:
        Entry(Logger& logger, LogLevel level, std::string_view event);
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept;
        Entry& operator=(Entry&& other) = delete;
        ~Entry();

        Entry& field(Field field_value);

        template <typename T>
        Entry& field(std::string key, T&& value, std::string_view message = {}) {
            fields_.emplace_back(std::move(key), std::forward<T>(value), message);
            return *this;
        }

        void emit();

    private:
        Logger* logger_ = nullptr;
        LogLevel level_ = LogLevel::Info;
        std::string event_;
        std::vector<Field> fields_;
        bool emitted_ = false;
    };

    static Logger& instance();

    void init(LogLevel default_level = LogLevel::Info, std::filesystem::path log_dir = "runtime/logs",
              bool also_stderr = true);
    void set_level(LogLevel level);
    [[nodiscard]] LogLevel level() const;

    Entry trace(std::string_view event);
    Entry debug(std::string_view event);
    Entry info(std::string_view event);
    Entry warn(std::string_view event);
    Entry error(std::string_view event);

private:
    Logger() = default;

    void log(LogLevel level, std::string_view event, std::span<const Field> fields);
    void ensure_initialized_locked();
    void rotate_file_if_needed_locked(std::string_view date, std::string_view timestamp);
    void write_line_locked(std::string_view line);

    [[nodiscard]] bool should_log(LogLevel level) const;

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::filesystem::path log_dir_ = "runtime/logs";
    bool also_stderr_ = true;
    bool force_stderr_only_ = false;
    std::string current_date_;
    std::ofstream file_;
    bool initialized_ = false;
};

}  // namespace nebula::common

#endif  // NEBULA_COMMON_LOGGER_HPP
