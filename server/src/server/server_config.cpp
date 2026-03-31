#include "nebula/server/server_config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nebula/config/toml_parser.hpp"

namespace nebula::server {

namespace {

using Table = std::unordered_map<std::string, config::TomlValue>;

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
};

std::string to_lower(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

void fail(ServerConfigLoadResult& result, std::string error, std::size_t error_line = 0) {
    result.ok = false;
    result.error = std::move(error);
    result.error_line = error_line;
}

const config::TomlValue* find_value(const Table& table, std::string_view key) {
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
    return false;
}

template <typename IntType>
bool assign_non_negative_integer(const Table& table, std::string_view key, IntType& target,
                                 ServerConfigLoadResult& result) {
    const config::TomlValue* raw = find_value(table, key);
    if (raw == nullptr) {
        return true;
    }

    const auto* value = std::get_if<std::int64_t>(&raw->value);
    if (value == nullptr) {
        fail(result, std::format("type_mismatch:{}", key), raw->line);
        return false;
    }

    if (*value < 0) {
        fail(result, std::format("negative_not_allowed:{}", key), raw->line);
        return false;
    }

    if (static_cast<std::uint64_t>(*value) > static_cast<std::uint64_t>(std::numeric_limits<IntType>::max())) {
        fail(result, std::format("value_out_of_range:{}", key), raw->line);
        return false;
    }

    target = static_cast<IntType>(*value);
    return true;
}

bool assign_bool_value(const Table& table, std::string_view key, bool& target, ServerConfigLoadResult& result) {
    const config::TomlValue* raw = find_value(table, key);
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
    const config::TomlValue* raw = find_value(table, key);
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
    const config::TomlParseResult parsed = config::parse_toml(text);
    if (!parsed.ok) {
        fail(result, std::format("parse_toml_failed:{}", parsed.error), parsed.error_line);
        return false;
    }

    table = parsed.table;
    return validate_known_keys(table, result);
}

bool apply_server_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    const config::TomlValue* port_raw = find_value(table, "server.port");
    if (!assign_non_negative_integer(table, "server.port", config.port, result)) {
        return false;
    }
    if (port_raw != nullptr && config.port == 0U) {
        fail(result, "value_out_of_range:server.port", port_raw->line);
        return false;
    }
    if (!assign_non_negative_integer(table, "server.backlog", config.backlog, result)) {
        return false;
    }
    if (!assign_non_negative_integer(table, "server.max_connections", config.max_connections, result)) {
        return false;
    }
    if (!assign_non_negative_integer(table, "server.sub_reactor_count", config.sub_reactor_count, result)) {
        return false;
    }
    if (!assign_non_negative_integer(table, "server.worker_thread_count", config.worker_thread_count, result)) {
        return false;
    }
    normalize_server_thread_counts(config);
    return assign_bool_value(table, "server.manage_signals", config.manage_signals, result);
}

bool apply_logger_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    const config::TomlValue* log_level = find_value(table, "logger.level");
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
    if (!assign_non_negative_integer(table, "timeouts.read_timeout_ms", read_timeout_ms, result)) {
        return false;
    }
    config.read_timeout = std::chrono::milliseconds(read_timeout_ms);

    std::int64_t graceful_shutdown_timeout_ms = config.graceful_shutdown_timeout.count();
    if (!assign_non_negative_integer(table, "timeouts.graceful_shutdown_timeout_ms", graceful_shutdown_timeout_ms,
                                     result)) {
        return false;
    }
    config.graceful_shutdown_timeout = std::chrono::milliseconds(graceful_shutdown_timeout_ms);
    return true;
}

bool apply_limit_values(const Table& table, ServerConfig& config, ServerConfigLoadResult& result) {
    if (!assign_non_negative_integer(table, "limits.max_header_bytes", config.max_header_bytes, result)) {
        return false;
    }
    if (!assign_non_negative_integer(table, "limits.max_request_target_bytes", config.max_request_target_bytes,
                                     result)) {
        return false;
    }
    return assign_non_negative_integer(table, "limits.max_body_bytes", config.max_body_bytes, result);
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
    return apply_limit_values(table, config, result);
}

}  // namespace

std::string_view to_string(ServerConfigSource source) {
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

}  // namespace nebula::server
