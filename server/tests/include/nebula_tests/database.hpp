#ifndef NEBULA_TESTS_DATABASE_HPP
#define NEBULA_TESTS_DATABASE_HPP

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/database/config.hpp"
#include "nebula/database/connection_info.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula_tests/common.hpp"

namespace nebula::test::database {

namespace detail {

inline std::vector<std::string> missing_database_test_env_names() {
    constexpr std::array<const char*, 5> required_env_names = {
        "NEBULA_TEST_DATABASE_HOST", "NEBULA_TEST_DATABASE_PORT",     "NEBULA_TEST_DATABASE_NAME",
        "NEBULA_TEST_DATABASE_USER", "NEBULA_TEST_DATABASE_PASSWORD",
    };

    std::vector<std::string> missing;
    for (const char* name : required_env_names) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            missing.emplace_back(name);
        }
    }
    return missing;
}

inline std::string format_missing_database_test_env_error(const std::vector<std::string>& missing) {
    std::string error = "missing_required_env:";
    for (std::size_t idx = 0; idx < missing.size(); ++idx) {
        if (idx > 0U) {
            error.push_back(',');
        }
        error.append(missing[idx]);
    }
    return error;
}

inline const char* required_database_test_env(std::string_view name) {
    const std::string env_name(name);
    const char* value = std::getenv(env_name.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("missing_required_env:" + env_name);
    }
    return value;
}

inline std::uint16_t parse_database_test_port(const std::string& value) {
    std::size_t parsed_size = 0;
    int port = 0;
    try {
        port = std::stoi(value, &parsed_size);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid_env:NEBULA_TEST_DATABASE_PORT");
    }
    if (parsed_size != value.size() || port <= 0 || port > 65535) {
        throw std::runtime_error("invalid_env:NEBULA_TEST_DATABASE_PORT");
    }
    return static_cast<std::uint16_t>(port);
}

}  // namespace detail

inline void require_database_test_env() {
    const std::vector<std::string> missing = detail::missing_database_test_env_names();
    if (!missing.empty()) {
        throw std::runtime_error(detail::format_missing_database_test_env_error(missing));
    }
}

inline ::nebula::database::DatabaseConfig build_test_database_config(const std::size_t max_connections = 4U,
                                                                     const std::int64_t connect_timeout_s = 3,
                                                                     const std::int64_t acquire_timeout_ms = 3000) {
    require_database_test_env();

    return ::nebula::database::DatabaseConfig{
        .host = detail::required_database_test_env("NEBULA_TEST_DATABASE_HOST"),
        .port = detail::parse_database_test_port(detail::required_database_test_env("NEBULA_TEST_DATABASE_PORT")),
        .name = detail::required_database_test_env("NEBULA_TEST_DATABASE_NAME"),
        .user = detail::required_database_test_env("NEBULA_TEST_DATABASE_USER"),
        .password_env = "",
        .password = detail::required_database_test_env("NEBULA_TEST_DATABASE_PASSWORD"),
        .max_connections = max_connections,
        .connect_timeout_s = connect_timeout_s,
        .acquire_timeout_ms = acquire_timeout_ms,
    };
}

inline std::shared_ptr<::nebula::database::ConnectionPool> create_database_pool(
    const ::nebula::database::DatabaseConfig& config) {
    auto database_pool = ::nebula::database::ConnectionPool::create(config);
    expect_true(database_pool != nullptr, "database pool creation should succeed");
    return database_pool;
}

inline void truncate_database_tables(const ::nebula::database::DatabaseConfig& config) {
    pqxx::connection connection(::nebula::database::build_connection_info(config));
    pqxx::work tx(connection);
    tx.exec(
        "TRUNCATE TABLE storage_nodes, storage_upload_sessions, storage_objects, storage_download_tickets, users "
        "RESTART IDENTITY");
    tx.exec("ALTER SEQUENCE users_user_id_seq RESTART WITH 1");
    tx.commit();
}

}  // namespace nebula::test::database

#endif  // NEBULA_TESTS_DATABASE_HPP
