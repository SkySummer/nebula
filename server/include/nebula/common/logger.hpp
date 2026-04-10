#ifndef NEBULA_COMMON_LOGGER_HPP
#define NEBULA_COMMON_LOGGER_HPP

#include <concepts>
#include <cstdint>
#include <exception>
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
    Fatal,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

enum class LogDomain : std::uint8_t {
    Auth,
    Common,
    Http,
    Server,
    User,
    Test,
};

[[nodiscard]] std::string_view to_string(LogDomain domain) noexcept;

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
        Entry() noexcept = default;
        Entry(Logger& logger, LogLevel level, std::string_view event);
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept;
        Entry& operator=(Entry&& other) = delete;
        ~Entry() noexcept;

        Entry& field(Field field_value) noexcept;

        template <typename T>
        Entry& field(std::string key, T&& value, std::string_view message = {}) noexcept {
            try {
                fields_.emplace_back(std::move(key), std::forward<T>(value), message);
            } catch (const std::exception& error) {
                report_field_append_error(error.what());
            } catch (...) {
                report_field_append_error("unknown");
            }
            return *this;
        }

        void emit() noexcept;

    private:
        static void report_field_append_error(const char* error) noexcept;

        Logger* logger_ = nullptr;
        LogLevel level_ = LogLevel::Info;
        std::string event_;
        std::vector<Field> fields_;
        bool emitted_ = false;
    };

    static Logger& instance();

    void initialize(LogLevel default_level = LogLevel::Info, std::filesystem::path log_dir = "runtime/logs",
                    bool also_stderr = true) noexcept;
    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    Entry trace(LogDomain domain, std::string_view event) noexcept;
    Entry debug(LogDomain domain, std::string_view event) noexcept;
    Entry info(LogDomain domain, std::string_view event) noexcept;
    Entry warn(LogDomain domain, std::string_view event) noexcept;
    Entry error(LogDomain domain, std::string_view event) noexcept;
    Entry fatal(LogDomain domain, std::string_view event) noexcept;

private:
    Logger() = default;

    Entry trace(std::string_view event) noexcept;
    Entry debug(std::string_view event) noexcept;
    Entry info(std::string_view event) noexcept;
    Entry warn(std::string_view event) noexcept;
    Entry error(std::string_view event) noexcept;
    Entry fatal(std::string_view event) noexcept;

    void log(LogLevel level, std::string_view event, std::span<const Field> fields) noexcept;
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
