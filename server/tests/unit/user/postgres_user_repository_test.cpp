#include <cstdint>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula/user/user_repository.hpp"
#include "nebula_tests/test_support.hpp"
#include "nebula_tests/test_support_database.hpp"

namespace {

using nebula::common::PostgresConnectionPool;
using nebula::common::PostgresConnectionPoolOptions;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::database::load_postgres_pool_test_options;
using nebula::testsupport::database::validate_database_test_env;
using nebula::user::allocate_user_id;
using nebula::user::check_user_schema_ready;
using nebula::user::create_user;
using nebula::user::find_user_by_id;
using nebula::user::find_user_by_username;
using nebula::user::UserAllocateIdStatus;
using nebula::user::UserCreateResult;
using nebula::user::UserFindStatus;
using nebula::user::UserInfo;

PostgresConnectionPoolOptions require_test_options() {
    const std::optional<PostgresConnectionPoolOptions> options = load_postgres_pool_test_options();
    if (!options.has_value()) {
        nebula::testsupport::fail("postgres test database env should be configured before running tests");
    }
    return options.value();
}

void truncate_users(const PostgresConnectionPoolOptions& options) {
    pqxx::connection connection(nebula::common::build_connection_info(options));
    pqxx::work tx(connection);
    tx.exec("TRUNCATE TABLE users RESTART IDENTITY");
    tx.exec("ALTER SEQUENCE users_user_id_seq RESTART WITH 1");
    tx.commit();
}

void test_repository_initialize_fails_when_connection_unreachable() {
    PostgresConnectionPoolOptions options{
        .host = "127.0.0.1",
        .port = 1,
        .database = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_ms = 1000,
        .acquire_timeout_ms = 1000,
    };

    const PostgresConnectionPool::InitializeStatus status = PostgresConnectionPool::instance().initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::ConnectionCreateFailed,
                 "postgres pool init should fail on unreachable connection");

    const bool schema_ready = check_user_schema_ready();
    expect_true(!schema_ready, "user schema check should fail when pool is unavailable");
}

void test_repository_crud_with_database() {
    const PostgresConnectionPoolOptions options = require_test_options();

    const PostgresConnectionPool::InitializeStatus status = PostgresConnectionPool::instance().initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize with valid test database");

    const bool schema_ready = check_user_schema_ready();
    expect_true(schema_ready, "user schema check should succeed with valid test database");
    if (!schema_ready) {
        return;
    }
    truncate_users(options);

    const auto first_id = allocate_user_id();
    expect_equal(first_id.status, UserAllocateIdStatus::Allocated, "allocate_user_id should succeed");
    expect_equal(first_id.user_id, static_cast<std::int64_t>(1), "first allocated user id should be 1");

    UserInfo user;
    user.user_id = first_id.user_id;
    user.username = "Alice_1";
    user.password_hash = "hash_1";
    user.created_at_s = 1700000000;
    expect_equal(create_user(user), UserCreateResult::Created, "create_user should insert user");

    const auto duplicate_id = allocate_user_id();
    expect_equal(duplicate_id.status, UserAllocateIdStatus::Allocated, "allocate_user_id should keep succeeding");
    UserInfo duplicate = user;
    duplicate.user_id = duplicate_id.user_id;
    expect_equal(create_user(duplicate), UserCreateResult::DuplicateUsername,
                 "create_user should reject duplicate username");

    UserInfo duplicate_user_id = user;
    duplicate_user_id.username = "Alice_2";
    expect_equal(create_user(duplicate_user_id), UserCreateResult::InternalError,
                 "create_user should not map duplicate user_id to duplicate username");

    const auto found_username = find_user_by_username("Alice_1");
    expect_equal(found_username.status, UserFindStatus::Found, "find_by_username should return found");
    expect_equal(found_username.info.user_id, first_id.user_id, "find_by_username should map to persisted user id");

    const auto missing_duplicate_user_id_username = find_user_by_username("Alice_2");
    expect_equal(missing_duplicate_user_id_username.status, UserFindStatus::NotFound,
                 "duplicate user_id failure should not insert new username");

    const auto found_user_id = find_user_by_id(first_id.user_id);
    expect_equal(found_user_id.status, UserFindStatus::Found, "find_by_user_id should return found");
    expect_equal(found_user_id.info.username, std::string("Alice_1"), "find_by_user_id should map username");

    const auto missing_username = find_user_by_username("Unknown");
    expect_equal(missing_username.status, UserFindStatus::NotFound, "unknown username should return not_found");
    const auto missing_user_id = find_user_by_id(999999);
    expect_equal(missing_user_id.status, UserFindStatus::NotFound, "unknown user id should return not_found");
}

int run_postgres_user_repository_tests() {
    const std::optional<std::string> env_error = validate_database_test_env();
    if (env_error.has_value()) {
        std::cerr << "[SKIP] postgres user repository test precheck skipped: error=" << *env_error << '\n';
        return nebula::testsupport::database::kDatabaseTestSkipReturnCode;
    }

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"postgres schema check fails when connection unreachable",
         test_repository_initialize_fails_when_connection_unreachable},
        {"postgres repository crud with schema ready", test_repository_crud_with_database},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_postgres_user_repository_tests);
}
