#ifndef NEBULA_TESTS_TEST_SUPPORT_DATABASE_HPP
#define NEBULA_TESTS_TEST_SUPPORT_DATABASE_HPP

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "nebula/common/postgres_connection_pool.hpp"

namespace nebula::testsupport::database {

inline constexpr int kDatabaseTestSkipReturnCode = 77;

inline std::string env_or_default(const char* name, std::string default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    return value;
}

inline std::optional<std::string> validate_database_test_env() {
    const char* database = std::getenv("NEBULA_TEST_DATABASE_NAME");
    const char* user = std::getenv("NEBULA_TEST_DATABASE_USER");
    const char* password = std::getenv("NEBULA_TEST_DATABASE_PASSWORD");

    std::vector<std::string> missing;
    if (database == nullptr || *database == '\0') {
        missing.emplace_back("NEBULA_TEST_DATABASE_NAME");
    }
    if (user == nullptr || *user == '\0') {
        missing.emplace_back("NEBULA_TEST_DATABASE_USER");
    }
    if (password == nullptr || *password == '\0') {
        missing.emplace_back("NEBULA_TEST_DATABASE_PASSWORD");
    }

    if (missing.empty()) {
        return std::nullopt;
    }

    std::string error = "missing_required_env";
    error.push_back(':');
    for (std::size_t idx = 0; idx < missing.size(); ++idx) {
        if (idx > 0U) {
            error.push_back(',');
        }
        error.append(missing[idx]);
    }
    return error;
}

inline std::optional<nebula::common::PostgresConnectionPoolOptions> load_postgres_pool_test_options(
    const std::size_t max_connections = 4U, const std::int64_t connect_timeout_ms = 3000,
    const std::int64_t acquire_timeout_ms = 3000) {
    const char* database = std::getenv("NEBULA_TEST_DATABASE_NAME");
    const char* user = std::getenv("NEBULA_TEST_DATABASE_USER");
    const char* password = std::getenv("NEBULA_TEST_DATABASE_PASSWORD");
    if (database == nullptr || *database == '\0' || user == nullptr || *user == '\0' || password == nullptr ||
        *password == '\0') {
        return std::nullopt;
    }

    nebula::common::PostgresConnectionPoolOptions options;
    options.host = env_or_default("NEBULA_TEST_DATABASE_HOST", "127.0.0.1");
    options.port = static_cast<std::uint16_t>(std::stoi(env_or_default("NEBULA_TEST_DATABASE_PORT", "5432")));
    options.database = database;
    options.user = user;
    options.password = password;
    options.max_connections = max_connections;
    options.connect_timeout_ms = connect_timeout_ms;
    options.acquire_timeout_ms = acquire_timeout_ms;
    return options;
}

}  // namespace nebula::testsupport::database

#endif  // NEBULA_TESTS_TEST_SUPPORT_DATABASE_HPP
