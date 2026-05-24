#include "nebula/database/config.hpp"

#include <cstdlib>
#include <expected>
#include <limits>
#include <string_view>

#include "nebula/common/log/logger.hpp"

namespace nebula::database {

namespace {

enum class EnvReadError : std::uint8_t {
    EmptyName,
    NotSet,
    EmptyValue,
};

std::expected<std::string, EnvReadError> read_env_value(std::string_view env_name) {
    if (env_name.empty()) {
        return std::unexpected(EnvReadError::EmptyName);
    }

    const std::string env_name_string(env_name);
    const char* value = std::getenv(env_name_string.c_str());
    if (value == nullptr) {
        return std::unexpected(EnvReadError::NotSet);
    }

    if (*value == '\0') {
        return std::unexpected(EnvReadError::EmptyValue);
    }

    return std::string(value);
}

}  // namespace

DatabaseConfig& DatabaseConfig::normalize() & {
    if (!password.empty()) {
        return *this;
    }

    const auto resolved_password = read_env_value(password_env);
    if (!resolved_password.has_value()) {
        switch (resolved_password.error()) {
            case EnvReadError::EmptyName:
                common::Logger::instance()
                    .error("database password resolve failed")
                    .field("key", "database.password_env")
                    .field("error", "empty_value");
                return *this;
            case EnvReadError::NotSet:
                common::Logger::instance()
                    .error("database password resolve failed")
                    .field("key", "database.password_env")
                    .field("env_name", password_env)
                    .field("error", "env_not_set");
                return *this;
            case EnvReadError::EmptyValue:
                common::Logger::instance()
                    .error("database password resolve failed")
                    .field("key", "database.password_env")
                    .field("env_name", password_env)
                    .field("error", "env_empty");
                return *this;
        }
    }

    password = *resolved_password;
    return *this;
}

DatabaseConfig&& DatabaseConfig::normalize() && {
    normalize();
    return std::move(*this);
}

bool DatabaseConfig::validate() const {
    bool ok = true;
    if (host.empty()) {
        common::Logger::instance()
            .error("database config value invalid")
            .field("key", "host")
            .field("error", "empty_value");
        ok = false;
    }
    if (port == 0U) {
        common::Logger::instance()
            .error("database config value out of range")
            .field("key", "port")
            .field("value", 0)
            .field("min_value", 1)
            .field("max_value", std::numeric_limits<std::uint16_t>::max());
        ok = false;
    }
    if (name.empty()) {
        common::Logger::instance()
            .error("database config value invalid")
            .field("key", "name")
            .field("error", "empty_value");
        ok = false;
    }
    if (user.empty()) {
        common::Logger::instance()
            .error("database config value invalid")
            .field("key", "user")
            .field("error", "empty_value");
        ok = false;
    }
    if (password.empty()) {
        common::Logger::instance()
            .error("database config value invalid")
            .field("key", "password")
            .field("error", "empty_value");
        ok = false;
    }
    if (max_connections == 0U || max_connections > kMaxDatabaseConnections) {
        common::Logger::instance()
            .error("database config value out of range")
            .field("key", "max_connections")
            .field("value", max_connections)
            .field("min_value", 1)
            .field("max_value", kMaxDatabaseConnections);
        ok = false;
    }
    if (connect_timeout_s <= 0 || connect_timeout_s > kMaxDatabaseConnectTimeoutS) {
        common::Logger::instance()
            .error("database config value out of range")
            .field("key", "connect_timeout_s")
            .field("value", connect_timeout_s)
            .field("min_value", 1)
            .field("max_value", kMaxDatabaseConnectTimeoutS);
        ok = false;
    }
    if (acquire_timeout_ms <= 0 || acquire_timeout_ms > kMaxDatabaseAcquireTimeoutMs) {
        common::Logger::instance()
            .error("database config value out of range")
            .field("key", "acquire_timeout_ms")
            .field("value", acquire_timeout_ms)
            .field("min_value", 1)
            .field("max_value", kMaxDatabaseAcquireTimeoutMs);
        ok = false;
    }
    return ok;
}

}  // namespace nebula::database
