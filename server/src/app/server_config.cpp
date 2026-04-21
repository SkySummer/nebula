#include "nebula/app/server_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nebula/common/toml_parser.hpp"

namespace nebula::app {

namespace {

constexpr std::int64_t kMaxAuthAccessTokenTtlSeconds = 86'400;
constexpr std::int64_t kMaxStorageUploadSessionTtlSeconds = 2'592'000;
constexpr std::int64_t kMaxStorageFileBytes = std::numeric_limits<std::int64_t>::max();

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
    "auth.jwt_secret_path",
    "auth.access_token_ttl_s",
    "auth.password_hash_iterations",
    "database.host",
    "database.port",
    "database.name",
    "database.user",
    "database.password_env",
    "database.max_connections",
    "database.connect_timeout_ms",
    "database.acquire_timeout_ms",
    "storage.root_dir",
    "storage.upload_session_ttl_s",
    "storage.max_file_bytes",
    "storage.max_file_kb",
    "storage.max_file_mb",
    "storage.max_file_gb",
};

std::string to_lower(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

bool path_contains_route_template_marker(std::string_view path) {
    return path.find('{') != std::string_view::npos || path.find('}') != std::string_view::npos;
}

void fail(ServerConfigLoadResult& result, std::string error, std::size_t error_line = 0) {
    result.ok = false;
    result.error = std::move(error);
    result.error_line = error_line;
}

const common::TomlValue* find_value(const Table& table, std::string_view key) {
    const auto it = table.find(std::string(key));
    if (it == table.end()) {
        return nullptr;
    }
    return &it->second;
}

bool parse_log_level(std::string_view text, common::LogLevel& level) {
    const std::string lowered = to_lower(text);
    if (lowered == "trace") {
        level = common::LogLevel::Trace;
        return true;
    }
    if (lowered == "debug") {
        level = common::LogLevel::Debug;
        return true;
    }
    if (lowered == "info") {
        level = common::LogLevel::Info;
        return true;
    }
    if (lowered == "warn" || lowered == "warning") {
        level = common::LogLevel::Warning;
        return true;
    }
    if (lowered == "error") {
        level = common::LogLevel::Error;
        return true;
    }
    if (lowered == "fatal") {
        level = common::LogLevel::Fatal;
        return true;
    }
    return false;
}

template <typename IntType>
bool assign_integer_in_range(const Table& table, std::string_view key, IntType& target, IntType min_value,
                             IntType max_value, ServerConfigLoadResult& result) {
    static_assert(std::is_integral_v<IntType>);

    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<std::int64_t>(&raw->value);
    if (value == nullptr) {
        fail(result, std::format("type_mismatch:{}", key), raw->line);
        return false;
    }

    if (min_value > max_value) {
        fail(result, std::format("invalid_range:{}", key), raw->line);
        return false;
    }

    if constexpr (std::is_unsigned_v<IntType>) {
        if (*value < 0) {
            fail(result, std::format("negative_not_allowed:{}", key), raw->line);
            return false;
        }

        const auto parsed = static_cast<std::uint64_t>(*value);
        const auto min_u64 = static_cast<std::uint64_t>(min_value);
        const auto max_u64 = static_cast<std::uint64_t>(max_value);
        if (parsed < min_u64 || parsed > max_u64) {
            fail(result, std::format("value_out_of_range:{}", key), raw->line);
            return false;
        }

        target = static_cast<IntType>(parsed);
        return true;
    } else {
        const auto min_i64 = static_cast<std::int64_t>(min_value);
        const auto max_i64 = static_cast<std::int64_t>(max_value);
        if (*value < min_i64 || *value > max_i64) {
            fail(result, std::format("value_out_of_range:{}", key), raw->line);
            return false;
        }

        target = static_cast<IntType>(*value);
        return true;
    }
}

template <typename IntType>
constexpr IntType min_toml_integer_for_type() {
    if constexpr (std::is_unsigned_v<IntType>) {
        return static_cast<IntType>(0);
    } else {
        constexpr auto k_toml_int_min = static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::min());
        constexpr auto k_type_min = static_cast<std::int64_t>(std::numeric_limits<IntType>::min());
        return static_cast<IntType>(std::max(k_type_min, k_toml_int_min));
    }
}

template <typename IntType>
constexpr IntType max_toml_integer_for_type() {
    constexpr auto k_toml_int_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto k_type_max = static_cast<std::uint64_t>(std::numeric_limits<IntType>::max());
    return static_cast<IntType>(std::min(k_type_max, k_toml_int_max));
}

template <typename IntType>
bool assign_integer(const Table& table, std::string_view key, IntType& target, ServerConfigLoadResult& result) {
    static_assert(std::is_integral_v<IntType>);
    return assign_integer_in_range(table, key, target, min_toml_integer_for_type<IntType>(),
                                   max_toml_integer_for_type<IntType>(), result);
}

template <typename IntType>
bool assign_integer_non_negative(const Table& table, std::string_view key, IntType& target,
                                 ServerConfigLoadResult& result) {
    static_assert(std::is_integral_v<IntType>);
    return assign_integer_in_range(table, key, target, static_cast<IntType>(0), max_toml_integer_for_type<IntType>(),
                                   result);
}

bool assign_bool_value(const Table& table, std::string_view key, bool& target, ServerConfigLoadResult& result) {
    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<bool>(&raw->value);
    if (value == nullptr) {
        fail(result, std::format("type_mismatch:{}", key), raw->line);
        return false;
    }

    target = *value;
    return true;
}

bool assign_string_value(const Table& table, std::string_view key, std::string& target,
                         ServerConfigLoadResult& result) {
    const common::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<std::string>(&raw->value);
    if (value == nullptr) {
        fail(result, std::format("type_mismatch:{}", key), raw->line);
        return false;
    }

    target = *value;
    return true;
}

bool read_config_file_text(const std::filesystem::path& path, std::string& text, ServerConfigLoadResult& result) {
    errno = 0;
    std::ifstream stream(path);
    if (!stream.is_open()) {
        const int err = errno;
        const std::string err_text = err == 0 ? "unknown" : std::system_category().message(err);
        fail(result, std::format("open_config_file_failed:errno={}:{}", err, err_text));
        return false;
    }

    errno = 0;
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) {
        const int err = errno;
        const std::string err_text = err == 0 ? "unknown" : std::system_category().message(err);
        fail(result, std::format("read_config_file_failed:errno={}:{}", err, err_text));
        return false;
    }
    return true;
}

bool validate_known_keys(const Table& table, ServerConfigLoadResult& result) {
    std::string first_unknown_key;
    std::size_t first_unknown_line = 0;

    for (const auto& [key, value] : table) {
        if (kKnownKeys.contains(key)) {
            continue;
        }

        const bool should_replace = first_unknown_key.empty() || value.line < first_unknown_line ||
                                    (value.line == first_unknown_line && key < first_unknown_key);
        if (should_replace) {
            first_unknown_key = key;
            first_unknown_line = value.line;
        }
    }

    if (!first_unknown_key.empty()) {
        fail(result, std::format("unknown_key:{}", first_unknown_key), first_unknown_line);
        return false;
    }

    return true;
}

bool parse_toml_table(std::string_view text, Table& table, ServerConfigLoadResult& result) {
    const common::TomlParseResult parsed = common::parse_toml(text);
    if (!parsed.ok) {
        fail(result, std::format("parse_toml_failed:{}", parsed.error), parsed.error_line);
        return false;
    }

    table = parsed.table;
    return validate_known_keys(table, result);
}

bool apply_server_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    const common::TomlValue* port_raw = find_value(table, "server.port");
    if (!assign_integer(table, "server.port", config.port, result)) {
        return false;
    }
    if (port_raw != nullptr && config.port == 0U) {
        fail(result, "value_out_of_range:server.port", port_raw->line);
        return false;
    }
    if (!assign_integer_non_negative(table, "server.backlog", config.backlog, result)) {
        return false;
    }
    if (!assign_integer(table, "server.max_connections", config.max_connections, result)) {
        return false;
    }
    if (!assign_integer(table, "server.sub_reactor_count", config.sub_reactor_count, result)) {
        return false;
    }
    if (!assign_integer(table, "server.worker_thread_count", config.worker_thread_count, result)) {
        return false;
    }
    config.normalize();
    return assign_bool_value(table, "server.manage_signals", config.manage_signals, result);
}

bool apply_logger_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    const common::TomlValue* log_level = find_value(table, "logger.level");
    if (log_level != nullptr) {
        const auto* text_value = std::get_if<std::string>(&log_level->value);
        if (text_value == nullptr) {
            fail(result, "type_mismatch:logger.level", log_level->line);
            return false;
        }
        if (!parse_log_level(*text_value, config.log_level)) {
            fail(result, std::format("invalid_log_level:{}", *text_value), log_level->line);
            return false;
        }
    }

    std::string log_dir = config.log_dir.string();
    if (!assign_string_value(table, "logger.dir", log_dir, result)) {
        return false;
    }
    config.log_dir = log_dir;
    return assign_bool_value(table, "logger.also_stderr", config.log_also_stderr, result);
}

bool apply_timeout_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    std::int64_t read_timeout_ms = config.read_timeout.count();
    if (!assign_integer_non_negative(table, "timeouts.read_timeout_ms", read_timeout_ms, result)) {
        return false;
    }
    config.read_timeout = std::chrono::milliseconds(read_timeout_ms);

    std::int64_t graceful_shutdown_timeout_ms = config.graceful_shutdown_timeout.count();
    if (!assign_integer_non_negative(table, "timeouts.graceful_shutdown_timeout_ms", graceful_shutdown_timeout_ms,
                                     result)) {
        return false;
    }
    config.graceful_shutdown_timeout = std::chrono::milliseconds(graceful_shutdown_timeout_ms);
    return true;
}

bool apply_limit_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!assign_integer(table, "limits.max_header_bytes", config.max_header_bytes, result)) {
        return false;
    }
    if (!assign_integer(table, "limits.max_request_target_bytes", config.max_request_target_bytes, result)) {
        return false;
    }
    return assign_integer(table, "limits.max_body_bytes", config.max_body_bytes, result);
}

bool apply_route_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!assign_bool_value(table, "routes.enable_healthz", config.enable_healthz, result)) {
        return false;
    }
    if (!assign_bool_value(table, "routes.enable_echo", config.enable_echo, result)) {
        return false;
    }
    if (!assign_bool_value(table, "routes.enable_root_default", config.enable_root_default, result)) {
        return false;
    }

    const common::TomlValue* root_default_path_raw = find_value(table, "routes.root_default_path");
    if (root_default_path_raw == nullptr) {
        if (config.enable_root_default) {
            fail(result, "invalid_value:routes.root_default_path:required_when_enable_root_default");
            return false;
        }
        return true;
    }

    const auto* root_default_path = std::get_if<std::string>(&root_default_path_raw->value);
    if (root_default_path == nullptr) {
        fail(result, "type_mismatch:routes.root_default_path", root_default_path_raw->line);
        return false;
    }
    if (root_default_path->empty()) {
        fail(result, "invalid_value:routes.root_default_path:empty_path", root_default_path_raw->line);
        return false;
    }
    if ((*root_default_path)[0] != '/') {
        fail(result, "invalid_value:routes.root_default_path:must_start_with_slash", root_default_path_raw->line);
        return false;
    }
    if (*root_default_path == "/") {
        fail(result, "invalid_value:routes.root_default_path:self_mapping_not_allowed", root_default_path_raw->line);
        return false;
    }
    if (path_contains_route_template_marker(*root_default_path)) {
        fail(result, "invalid_value:routes.root_default_path:path_template_not_allowed", root_default_path_raw->line);
        return false;
    }

    config.root_default_path = *root_default_path;
    return true;
}

bool assign_non_empty_string_value(const Table& table, std::string_view key, std::string& target,
                                   ServerConfigLoadResult& result) {
    if (!assign_string_value(table, key, target, result)) {
        return false;
    }
    if (!target.empty()) {
        return true;
    }

    const common::TomlValue* raw = find_value(table, key);
    fail(result, std::format("invalid_value:{}:empty_value", key), raw == nullptr ? 0 : raw->line);
    return false;
}

bool validate_database_password_env(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!assign_non_empty_string_value(table, "database.password_env", config.database_password_env, result)) {
        return false;
    }

    const char* env_password = std::getenv(config.database_password_env.c_str());
    if (env_password != nullptr && *env_password != '\0') {
        return true;
    }

    const common::TomlValue* password_env_raw = find_value(table, "database.password_env");
    fail(result, "invalid_value:database.password_env:env_not_set",
         password_env_raw == nullptr ? 0 : password_env_raw->line);
    return false;
}

bool apply_database_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!assign_non_empty_string_value(table, "database.host", config.database_host, result)) {
        return false;
    }
    if (!assign_integer_in_range(table, "database.port", config.database_port, static_cast<std::uint16_t>(1),
                                 max_toml_integer_for_type<std::uint16_t>(), result)) {
        return false;
    }
    if (!assign_non_empty_string_value(table, "database.name", config.database_name, result)) {
        return false;
    }
    if (!assign_non_empty_string_value(table, "database.user", config.database_user, result)) {
        return false;
    }
    if (!assign_integer_in_range(table, "database.max_connections", config.database_max_connections,
                                 static_cast<std::size_t>(1), static_cast<std::size_t>(1024), result)) {
        return false;
    }
    if (!assign_integer_in_range(table, "database.connect_timeout_ms", config.database_connect_timeout_ms,
                                 static_cast<std::int64_t>(1), static_cast<std::int64_t>(60'000), result)) {
        return false;
    }
    if (!assign_integer_in_range(table, "database.acquire_timeout_ms", config.database_acquire_timeout_ms,
                                 static_cast<std::int64_t>(1), static_cast<std::int64_t>(60'000), result)) {
        return false;
    }
    return validate_database_password_env(table, config, result);
}

bool apply_storage_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    std::string storage_root_dir = config.storage_root_dir.string();
    if (!assign_non_empty_string_value(table, "storage.root_dir", storage_root_dir, result)) {
        return false;
    }
    config.storage_root_dir = storage_root_dir;

    if (!assign_integer_in_range(table, "storage.upload_session_ttl_s", config.storage_upload_session_ttl_s,
                                 static_cast<std::int64_t>(1), kMaxStorageUploadSessionTtlSeconds, result)) {
        return false;
    }

    const common::TomlValue* max_file_bytes = find_value(table, "storage.max_file_bytes");
    const common::TomlValue* max_file_kb = find_value(table, "storage.max_file_kb");
    const common::TomlValue* max_file_mb = find_value(table, "storage.max_file_mb");
    const common::TomlValue* max_file_gb = find_value(table, "storage.max_file_gb");
    const std::size_t configured_count = (max_file_bytes == nullptr ? 0U : 1U) + (max_file_kb == nullptr ? 0U : 1U) +
                                         (max_file_mb == nullptr ? 0U : 1U) + (max_file_gb == nullptr ? 0U : 1U);
    if (configured_count == 0U) {
        return true;
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
        fail(result, "invalid_value:storage.max_file_size:multiple_units", error_line);
        return false;
    }

    std::int64_t size_value = config.storage_max_file_bytes;
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

    if (!assign_integer_in_range(table, key, size_value, std::int64_t{1}, kMaxStorageFileBytes / multiplier, result)) {
        return false;
    }
    config.storage_max_file_bytes = size_value * multiplier;
    return true;
}

bool apply_auth_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    const common::TomlValue* jwt_secret_path_raw = find_value(table, "auth.jwt_secret_path");
    if (jwt_secret_path_raw != nullptr) {
        const auto* jwt_secret_path = std::get_if<std::string>(&jwt_secret_path_raw->value);
        if (jwt_secret_path == nullptr) {
            fail(result, "type_mismatch:auth.jwt_secret_path", jwt_secret_path_raw->line);
            return false;
        }
        if (jwt_secret_path->empty()) {
            fail(result, "invalid_value:auth.jwt_secret_path:empty_value", jwt_secret_path_raw->line);
            return false;
        }
        config.auth_jwt_secret_path = *jwt_secret_path;
    }

    if (!assign_integer_in_range(table, "auth.access_token_ttl_s", config.auth_access_token_ttl_s,
                                 static_cast<std::int64_t>(0), kMaxAuthAccessTokenTtlSeconds, result)) {
        return false;
    }
    if (config.auth_access_token_ttl_s == 0) {
        const common::TomlValue* token_ttl_raw = find_value(table, "auth.access_token_ttl_s");
        fail(result, "invalid_value:auth.access_token_ttl_s:must_be_positive",
             token_ttl_raw == nullptr ? 0 : token_ttl_raw->line);
        return false;
    }

    if (!assign_integer_in_range(table, "auth.password_hash_iterations", config.auth_password_hash_iterations,
                                 auth::kMinPasswordHashIterations, auth::kMaxPasswordHashIterations, result)) {
        return false;
    }

    return true;
}

bool apply_table_to_config(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!apply_server_values(table, config, result)) {
        return false;
    }
    if (!apply_logger_values(table, config, result)) {
        return false;
    }
    if (!apply_timeout_values(table, config, result)) {
        return false;
    }
    if (!apply_limit_values(table, config, result)) {
        return false;
    }
    if (!apply_route_values(table, config, result)) {
        return false;
    }
    if (!apply_auth_values(table, config, result)) {
        return false;
    }
    if (!apply_database_values(table, config, result)) {
        return false;
    }
    if (!apply_storage_values(table, config, result)) {
        return false;
    }
    return true;
}

}  // namespace

std::size_t default_worker_thread_count() {
    const std::size_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0U) {
        return 1U;
    }
    return std::max<std::size_t>(1U, hardware / 2U);
}

std::size_t default_sub_reactor_count() {
    const std::size_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0U) {
        return 1U;
    }
    return std::max<std::size_t>(1U, hardware / 2U);
}

ServerConfig& ServerConfig::normalize() & {
    if (sub_reactor_count == 0U) {
        sub_reactor_count = default_sub_reactor_count();
    }
    if (worker_thread_count == 0U) {
        worker_thread_count = default_worker_thread_count();
    }
    return *this;
}

ServerConfig&& ServerConfig::normalize() && {
    normalize();
    return std::move(*this);
}

std::string_view to_string(ServerConfigSource source) noexcept {
    switch (source) {
        case ServerConfigSource::Default:
            return "default";
        case ServerConfigSource::File:
            return "file";
    }
    return "unknown";
}

ServerConfigLoadResult::ServerConfigLoadResult(const std::filesystem::path& path) {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        fail(*this, std::format("stat_config_file_failed:{}", exists_error.message()));
        return;
    }

    source = ServerConfigSource::File;
    if (!exists) {
        fail(*this, "config_file_not_found");
        return;
    }

    std::string text;
    if (!read_config_file_text(path, text, *this)) {
        return;
    }

    Table table;
    if (!parse_toml_table(text, table, *this)) {
        return;
    }

    ServerConfig parsed_config;
    if (!apply_table_to_config(table, parsed_config, *this)) {
        return;
    }

    config = std::move(parsed_config);
    ok = true;
}

}  // namespace nebula::app
