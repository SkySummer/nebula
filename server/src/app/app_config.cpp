#include "nebula/app/app_config.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula/common/codec/toml.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/file_io.hpp"

namespace nebula::app {

namespace {

using Table = std::unordered_map<std::string, common::TomlValue>;

const std::unordered_set<std::string> kKnownKeys = {
    "server.port",
    "server.backlog",
    "server.max_connections",
    "server.sub_reactor_count",
    "server.worker_thread_count",
    "server.manage_signals",
    "logger.level",
    "logger.dir",
    "logger.also_stderr",
    "timeouts.read_timeout_ms",
    "timeouts.graceful_shutdown_timeout_ms",
    "limits.max_header_bytes",
    "limits.max_request_target_bytes",
    "limits.max_body_bytes",
    "routes.enable_healthz",
    "routes.enable_echo",
    "routes.enable_root_default",
    "routes.root_default_path",
    "database.host",
    "database.port",
    "database.name",
    "database.user",
    "database.password_env",
    "database.max_connections",
    "database.connect_timeout_s",
    "database.acquire_timeout_ms",
    "auth.jwt_secret_path",
    "auth.access_token_ttl_s",
    "auth.password_hash_iterations",
    "storage.root_dir",
    "storage.upload_session_ttl_s",
    "storage.download_ticket_ttl_s",
    "storage.max_file_bytes",
    "storage.max_file_kb",
    "storage.max_file_mb",
    "storage.max_file_gb",
};

const common::TomlValue* find_value(const Table& table, std::string_view key) {
    const auto it = table.find(std::string(key));
    if (it == table.end()) {
        return nullptr;
    }
    return &it->second;
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
bool assign_integer_in_range(const std::filesystem::path& path, const Table& table, std::string_view key,
                             IntType& target, IntType min_value, IntType max_value) {
    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<std::int64_t>(&raw->value);
    if (value == nullptr) {
        common::Logger::instance()
            .error("app config type mismatch")
            .field("path", path)
            .field("key", key)
            .field("expected", "integer")
            .field("line", raw->line);
        return false;
    }

    if (min_value > max_value) {
        common::Logger::instance()
            .error("app config value invalid")
            .field("path", path)
            .field("key", key)
            .field("line", raw->line)
            .field("error", "invalid_range");
        return false;
    }

    if constexpr (std::is_unsigned_v<IntType>) {
        if (*value < 0) {
            common::Logger::instance()
                .error("app config value out of range")
                .field("path", path)
                .field("key", key)
                .field("value", *value)
                .field("min_value", min_value)
                .field("max_value", max_value)
                .field("line", raw->line);
            return false;
        }

        const auto parsed = static_cast<std::uint64_t>(*value);
        if (std::cmp_less(parsed, min_value) || std::cmp_greater(parsed, max_value)) {
            common::Logger::instance()
                .error("app config value out of range")
                .field("path", path)
                .field("key", key)
                .field("value", *value)
                .field("min_value", min_value)
                .field("max_value", max_value)
                .field("line", raw->line);
            return false;
        }

        target = static_cast<IntType>(parsed);
        return true;
    } else {
        const auto min_i64 = static_cast<std::int64_t>(min_value);
        const auto max_i64 = static_cast<std::int64_t>(max_value);
        if (*value < min_i64 || *value > max_i64) {
            common::Logger::instance()
                .error("app config value out of range")
                .field("path", path)
                .field("key", key)
                .field("value", *value)
                .field("min_value", min_value)
                .field("max_value", max_value)
                .field("line", raw->line);
            return false;
        }

        target = static_cast<IntType>(*value);
        return true;
    }
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
bool assign_integer_in_range(bool& ok, const std::filesystem::path& path, const Table& table, std::string_view key,
                             IntType& target, IntType min_value, IntType max_value) {
    const bool step_ok = assign_integer_in_range(path, table, key, target, min_value, max_value);
    ok = step_ok && ok;
    return step_ok;
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
constexpr IntType min_toml_integer() {
    if constexpr (std::is_unsigned_v<IntType>) {
        return IntType{0};
    } else {
        constexpr auto k_toml_int_min = static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::min());
        constexpr auto k_type_min = static_cast<std::int64_t>(std::numeric_limits<IntType>::min());
        return static_cast<IntType>(std::max(k_type_min, k_toml_int_min));
    }
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
constexpr IntType max_toml_integer() {
    constexpr auto k_toml_int_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto k_type_max = static_cast<std::uint64_t>(std::numeric_limits<IntType>::max());
    return static_cast<IntType>(std::min(k_type_max, k_toml_int_max));
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
bool assign_integer(const std::filesystem::path& path, const Table& table, std::string_view key, IntType& target) {
    return assign_integer_in_range(path, table, key, target, min_toml_integer<IntType>(), max_toml_integer<IntType>());
}

template <typename IntType>
    requires std::integral<IntType> && (!std::same_as<IntType, bool>)
bool assign_integer(bool& ok, const std::filesystem::path& path, const Table& table, std::string_view key,
                    IntType& target) {
    const bool step_ok = assign_integer(path, table, key, target);
    ok = step_ok && ok;
    return step_ok;
}

bool assign_bool_value(const std::filesystem::path& path, const Table& table, std::string_view key, bool& target) {
    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<bool>(&raw->value);
    if (value == nullptr) {
        common::Logger::instance()
            .error("app config type mismatch")
            .field("path", path)
            .field("key", key)
            .field("expected", "boolean")
            .field("line", raw->line);
        return false;
    }

    target = *value;
    return true;
}

bool assign_bool_value(bool& ok, const std::filesystem::path& path, const Table& table, std::string_view key,
                       bool& target) {
    const bool step_ok = assign_bool_value(path, table, key, target);
    ok = step_ok && ok;
    return step_ok;
}

bool assign_string_value(const std::filesystem::path& path, const Table& table, std::string_view key,
                         std::string& target) {
    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<std::string>(&raw->value);
    if (value == nullptr) {
        common::Logger::instance()
            .error("app config type mismatch")
            .field("path", path)
            .field("key", key)
            .field("expected", "string")
            .field("line", raw->line);
        return false;
    }

    target = *value;
    return true;
}

bool assign_string_value(bool& ok, const std::filesystem::path& path, const Table& table, std::string_view key,
                         std::string& target) {
    const bool step_ok = assign_string_value(path, table, key, target);
    ok = step_ok && ok;
    return step_ok;
}

bool read_config_file_text(const std::filesystem::path& path, std::string& text) {
    auto text_result = common::read_file(path);
    if (!text_result.has_value()) {
        switch (text_result.error()) {
            case common::ReadFileError::OpenFailed:
                common::Logger::instance()
                    .error("open app config failed")
                    .field("path", path)
                    .field("error", common::to_string(text_result.error()));
                return false;
            case common::ReadFileError::ReadFailed:
            case common::ReadFileError::TooLarge:
                common::Logger::instance()
                    .error("read app config failed")
                    .field("path", path)
                    .field("error", common::to_string(text_result.error()));
                return false;
        }
    }

    text = std::move(*text_result);
    return true;
}

bool validate_known_keys(const std::filesystem::path& path, const Table& table) {
    struct UnknownKey {
        std::size_t line = 0;
        std::string key;

        auto operator<=>(const UnknownKey&) const = default;
    };

    std::vector<UnknownKey> unknown_keys;
    for (const auto& [key, value] : table) {
        if (!kKnownKeys.contains(key)) {
            unknown_keys.push_back({.line = value.line, .key = key});
        }
    }
    std::ranges::sort(unknown_keys);

    for (const UnknownKey& unknown : unknown_keys) {
        common::Logger::instance()
            .error("app config unknown key")
            .field("path", path)
            .field("key", unknown.key)
            .field("line", unknown.line);
    }

    return unknown_keys.empty();
}

bool parse_toml_table(const std::filesystem::path& path, std::string_view text, Table& table) {
    const common::TomlParseResult parsed = common::parse_toml(text);
    if (!parsed.ok) {
        common::Logger::instance()
            .error("parse app config failed")
            .field("path", path)
            .field("line", parsed.error_line)
            .field("error", parsed.error);
        return false;
    }

    table = parsed.table;
    return true;
}

bool parse_server_config(const std::filesystem::path& path, const Table& table, server::ServerConfig& config) {
    bool ok = true;
    assign_integer(ok, path, table, "server.port", config.port);
    assign_integer(ok, path, table, "server.backlog", config.backlog);
    assign_integer(ok, path, table, "server.max_connections", config.max_connections);
    assign_integer(ok, path, table, "server.sub_reactor_count", config.sub_reactor_count);
    assign_integer(ok, path, table, "server.worker_thread_count", config.worker_thread_count);
    assign_bool_value(ok, path, table, "server.manage_signals", config.manage_signals);
    return ok;
}

bool parse_logger_config(const std::filesystem::path& path, const Table& table, common::LoggerConfig& config) {
    bool ok = true;
    const common::TomlValue* log_level = find_value(table, "logger.level");
    if (log_level != nullptr) {
        const auto* text_value = std::get_if<std::string>(&log_level->value);
        if (text_value == nullptr) {
            common::Logger::instance()
                .error("app config type mismatch")
                .field("path", path)
                .field("key", "logger.level")
                .field("expected", "string")
                .field("line", log_level->line);
            ok = false;
        } else {
            const std::optional<common::LogLevel> parsed_level = common::parse_log_level(*text_value);
            if (!parsed_level.has_value()) {
                common::Logger::instance()
                    .error("app config value invalid")
                    .field("path", path)
                    .field("key", "logger.level")
                    .field("value", *text_value)
                    .field("line", log_level->line)
                    .field("error", "invalid_log_level");
                ok = false;
            } else {
                config.level = *parsed_level;
            }
        }
    }

    std::string log_dir = config.dir.generic_string();
    assign_string_value(ok, path, table, "logger.dir", log_dir);
    config.dir = log_dir;
    assign_bool_value(ok, path, table, "logger.also_stderr", config.also_stderr);
    return ok;
}

bool parse_timeout_config(const std::filesystem::path& path, const Table& table, server::ServerTimeoutConfig& config) {
    bool ok = true;
    std::int64_t read_timeout_ms = config.read_timeout.count();
    assign_integer(ok, path, table, "timeouts.read_timeout_ms", read_timeout_ms);
    config.read_timeout = std::chrono::milliseconds(read_timeout_ms);

    std::int64_t graceful_shutdown_timeout_ms = config.graceful_shutdown_timeout.count();
    assign_integer(ok, path, table, "timeouts.graceful_shutdown_timeout_ms", graceful_shutdown_timeout_ms);
    config.graceful_shutdown_timeout = std::chrono::milliseconds(graceful_shutdown_timeout_ms);
    return ok;
}

bool parse_limit_config(const std::filesystem::path& path, const Table& table, http::HttpLimitsConfig& config) {
    bool ok = true;
    assign_integer(ok, path, table, "limits.max_header_bytes", config.max_header_bytes);
    assign_integer(ok, path, table, "limits.max_request_target_bytes", config.max_request_target_bytes);
    assign_integer(ok, path, table, "limits.max_body_bytes", config.max_body_bytes);
    return ok;
}

bool parse_route_config(const std::filesystem::path& path, const Table& table, RouteConfig& config) {
    bool ok = true;
    assign_bool_value(ok, path, table, "routes.enable_healthz", config.enable_healthz);
    assign_bool_value(ok, path, table, "routes.enable_echo", config.enable_echo);
    assign_bool_value(ok, path, table, "routes.enable_root_default", config.enable_root_default);
    assign_string_value(ok, path, table, "routes.root_default_path", config.root_default_path);
    return ok;
}

bool parse_database_config(const std::filesystem::path& path, const Table& table, database::DatabaseConfig& config) {
    bool ok = true;
    assign_string_value(ok, path, table, "database.host", config.host);
    assign_integer(ok, path, table, "database.port", config.port);
    assign_string_value(ok, path, table, "database.name", config.name);
    assign_string_value(ok, path, table, "database.user", config.user);
    assign_integer(ok, path, table, "database.max_connections", config.max_connections);
    assign_integer(ok, path, table, "database.connect_timeout_s", config.connect_timeout_s);
    assign_integer(ok, path, table, "database.acquire_timeout_ms", config.acquire_timeout_ms);
    assign_string_value(ok, path, table, "database.password_env", config.password_env);
    return ok;
}

bool parse_auth_config(const std::filesystem::path& path, const Table& table, auth::AuthConfig& config) {
    bool ok = true;
    std::string jwt_secret_path = config.jwt_secret_path.generic_string();
    assign_string_value(ok, path, table, "auth.jwt_secret_path", jwt_secret_path);
    config.jwt_secret_path = jwt_secret_path;
    assign_integer(ok, path, table, "auth.access_token_ttl_s", config.access_token_ttl_s);
    assign_integer(ok, path, table, "auth.password_hash_iterations", config.password_hash_iterations);
    return ok;
}

bool parse_storage_config(const std::filesystem::path& path, const Table& table, storage::StorageConfig& config) {
    bool ok = true;
    std::string storage_root_dir = config.root_dir.generic_string();
    assign_string_value(ok, path, table, "storage.root_dir", storage_root_dir);
    config.root_dir = storage_root_dir;

    assign_integer(ok, path, table, "storage.upload_session_ttl_s", config.upload_session_ttl_s);
    assign_integer(ok, path, table, "storage.download_ticket_ttl_s", config.download_ticket_ttl_s);

    const common::TomlValue* max_file_bytes = find_value(table, "storage.max_file_bytes");
    const common::TomlValue* max_file_kb = find_value(table, "storage.max_file_kb");
    const common::TomlValue* max_file_mb = find_value(table, "storage.max_file_mb");
    const common::TomlValue* max_file_gb = find_value(table, "storage.max_file_gb");
    const std::size_t configured_count = (max_file_bytes == nullptr ? 0U : 1U) + (max_file_kb == nullptr ? 0U : 1U) +
                                         (max_file_mb == nullptr ? 0U : 1U) + (max_file_gb == nullptr ? 0U : 1U);
    if (configured_count == 0U) {
        return ok;
    }
    if (configured_count > 1U) {
        std::size_t error_line = std::numeric_limits<std::size_t>::max();
        const std::array<const common::TomlValue*, 4> size_values = {max_file_bytes, max_file_kb, max_file_mb,
                                                                     max_file_gb};
        for (const common::TomlValue* value : size_values) {
            if (value != nullptr) {
                error_line = std::min(error_line, value->line);
            }
        }
        common::Logger::instance()
            .error("app config value invalid")
            .field("path", path)
            .field("key", "storage.max_file_size")
            .field("line", error_line)
            .field("error", "multiple_units");
        return false;
    }

    std::int64_t size_value = config.max_file_bytes;
    std::int64_t multiplier = 1;
    std::string_view key = "storage.max_file_bytes";
    if (max_file_kb != nullptr) {
        multiplier = 1024LL;
        key = "storage.max_file_kb";
    } else if (max_file_mb != nullptr) {
        multiplier = 1024LL * 1024;
        key = "storage.max_file_mb";
    } else if (max_file_gb != nullptr) {
        multiplier = 1024LL * 1024 * 1024;
        key = "storage.max_file_gb";
    }

    const std::int64_t min_size_value = std::numeric_limits<std::int64_t>::min() / multiplier;
    const std::int64_t max_size_value = std::numeric_limits<std::int64_t>::max() / multiplier;
    if (!assign_integer_in_range(path, table, key, size_value, min_size_value, max_size_value)) {
        return false;
    }

    config.max_file_bytes = size_value * multiplier;
    return ok;
}

bool parse_app_config(const std::filesystem::path& path, const Table& table, AppConfig& config) {
    bool ok = true;
    if (!parse_server_config(path, table, config.server)) {
        ok = false;
    }
    if (!parse_logger_config(path, table, config.logger)) {
        ok = false;
    }
    if (!parse_timeout_config(path, table, config.timeouts)) {
        ok = false;
    }
    if (!parse_limit_config(path, table, config.limits)) {
        ok = false;
    }
    if (!parse_route_config(path, table, config.routes)) {
        ok = false;
    }
    if (!parse_database_config(path, table, config.database)) {
        ok = false;
    }
    if (!parse_auth_config(path, table, config.auth)) {
        ok = false;
    }
    if (!parse_storage_config(path, table, config.storage)) {
        ok = false;
    }
    return ok;
}

}  // namespace

AppConfig& AppConfig::normalize() & {
    server.normalize();
    database.normalize();
    return *this;
}

AppConfig&& AppConfig::normalize() && {
    normalize();
    return std::move(*this);
}

bool AppConfig::validate() const {
    bool ok = true;
    ok = server.validate() && ok;
    ok = routes.validate() && ok;
    ok = database.validate() && ok;
    ok = auth.validate() && ok;
    ok = storage.validate() && ok;
    return ok;
}

std::string_view to_string(AppConfigSource source) noexcept {
    switch (source) {
        case AppConfigSource::Default:
            return "default";
        case AppConfigSource::File:
            return "file";
    }
    std::unreachable();
}

AppConfigLoadResult load_app_config(const std::filesystem::path& path) {
    AppConfigLoadResult result;
    result.source = AppConfigSource::File;

    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        result.ok = false;
        common::Logger::instance()
            .error("stat app config failed")
            .field("path", path)
            .field("error", exists_error.message());
        return result;
    }

    if (!exists) {
        result.ok = false;
        common::Logger::instance()
            .error("app config file not found")
            .field("path", path)
            .field("error", "config_file_not_found");
        return result;
    }

    std::string text;
    if (!read_config_file_text(path, text)) {
        result.ok = false;
        return result;
    }

    Table table;
    if (!parse_toml_table(path, text, table)) {
        result.ok = false;
        return result;
    }

    AppConfig parsed_config;
    bool ok = validate_known_keys(path, table);
    if (!parse_app_config(path, table, parsed_config)) {
        ok = false;
    }

    parsed_config.normalize();
    if (!parsed_config.validate()) {
        ok = false;
    }

    if (!ok) {
        result.ok = false;
        return result;
    }

    result.config = std::move(parsed_config);
    result.ok = true;
    return result;
}

}  // namespace nebula::app
