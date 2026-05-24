#ifndef NEBULA_COMMON_LOG_LOGGER_HPP
#define NEBULA_COMMON_LOG_LOGGER_HPP

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
#include <variant>
#include <vector>

#include "nebula/common/log/types.hpp"

namespace nebula::common {

template <typename T>
concept StringLike = std::convertible_to<const std::remove_cvref_t<T>&, std::string_view>;

using FieldValue = std::variant<std::string, std::nullptr_t, bool, std::int64_t, double>;

struct Field {
    Field(std::string key_in, std::string value_in) : key(std::move(key_in)), value(std::move(value_in)) {}

    Field(std::string key_in, std::string_view value_in) : key(std::move(key_in)), value(std::string(value_in)) {}

    Field(std::string key_in, const char* value_in)
        : key(std::move(key_in)),
          value(value_in == nullptr ? FieldValue(nullptr) : FieldValue(std::string(value_in))) {}

    Field(std::string key_in, const std::filesystem::path& value_in)
        : key(std::move(key_in)), value(value_in.generic_string()) {}

    Field(std::string key_in, std::nullptr_t) : key(std::move(key_in)), value(nullptr) {}

    Field(std::string key_in, bool value_in) : key(std::move(key_in)), value(value_in) {}

    template <typename T>
        requires(std::integral<T> && !std::same_as<T, bool>)
    Field(std::string key_in, T value_in) : key(std::move(key_in)), value(make_integral_value(value_in)) {}

    template <typename T>
        requires std::floating_point<T>
    Field(std::string key_in, T value_in) : key(std::move(key_in)), value(static_cast<double>(value_in)) {}

    template <typename T>
        requires(std::formattable<std::remove_cvref_t<T>, char> && !StringLike<T> &&
                 !std::same_as<std::remove_cvref_t<T>, std::filesystem::path> &&
                 !std::integral<std::remove_cvref_t<T>> && !std::floating_point<std::remove_cvref_t<T>>)
    Field(std::string key_in, T&& value_in)
        : key(std::move(key_in)), value(std::format("{}", std::forward<T>(value_in))) {}

    std::string key;
    FieldValue value;

private:
    template <std::integral T>
    static FieldValue make_integral_value(T value_in) {
        if constexpr (std::signed_integral<T>) {
            return static_cast<std::int64_t>(value_in);
        } else {
            if (std::in_range<std::int64_t>(value_in)) {
                return static_cast<std::int64_t>(value_in);
            }
            return std::format("{}", value_in);
        }
    }
};

class Logger {
public:
    class Entry {
    public:
        Entry() noexcept = default;
        Entry(Logger& logger, LogLevel level, std::string_view event);
        ~Entry() noexcept;

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept;
        Entry& operator=(Entry&& other) = delete;

        Entry& field(Field field_value) noexcept;

        template <typename T>
        Entry& field(std::string_view key, T&& value) noexcept {
            try {
                fields_.emplace_back(std::string(key), std::forward<T>(value));
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

    void initialize(LoggerConfig config) noexcept;

    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    Entry trace(std::string_view event) noexcept;
    Entry debug(std::string_view event) noexcept;
    Entry info(std::string_view event) noexcept;
    Entry warn(std::string_view event) noexcept;
    Entry error(std::string_view event) noexcept;
    Entry fatal(std::string_view event) noexcept;

private:
    Logger() = default;

    void log(LogLevel level, std::string_view event, std::span<const Field> fields) noexcept;
    void rotate_file_if_needed_locked(std::string_view date, std::string_view timestamp);
    void write_line_locked(std::string_view line);

    [[nodiscard]] bool should_log(LogLevel level) const;

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Trace;
    bool file_enabled_ = false;
    bool stderr_enabled_ = true;
    std::filesystem::path log_dir_;
    std::ofstream file_;
    std::string current_date_;
};

}  // namespace nebula::common

#endif  // NEBULA_COMMON_LOG_LOGGER_HPP
