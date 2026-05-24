#include "nebula/database/connection_pool.hpp"

#include <chrono>
#include <optional>
#include <vector>

#include "nebula/database/config.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"

namespace {

using namespace nebula;

void test_connection_pool_create_fails_when_max_connections_invalid() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 0,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 1000,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when max_connections is invalid");
}

void test_connection_pool_create_fails_when_connect_timeout_invalid() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 0,
        .acquire_timeout_ms = 1000,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when connect timeout is invalid");
}

void test_connection_pool_create_fails_when_connect_timeout_exceeds_config_limit() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 61,
        .acquire_timeout_ms = 1000,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when connect timeout exceeds config limit");
}

void test_connection_pool_create_fails_when_acquire_timeout_invalid() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 0,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when acquire timeout is invalid");
}

void test_connection_pool_create_fails_when_acquire_timeout_exceeds_config_limit() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 60'001,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when acquire timeout exceeds config limit");
}

void test_connection_pool_create_fails_when_max_connections_exceeds_config_limit() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1025,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 1000,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when max connections exceeds config limit");
}

void test_connection_pool_create_fails_when_connection_unreachable() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 1,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 1000,
    };

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool should fail create when database is unreachable");
}

void test_connection_pool_create_from_database_config_succeeds_with_valid_test_env() {
    const database::DatabaseConfig config = test::database::build_test_database_config();
    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool != nullptr, "postgres pool should create from database config");
}

void test_connection_pool_acquire_returns_open_connection_after_create() {
    const auto pool = database::ConnectionPool::create(test::database::build_test_database_config());
    test::expect_true(pool != nullptr, "postgres pool should create with valid test database");

    const auto lease = pool->acquire_lease();
    if (!lease.has_value()) {
        test::fail("acquire_lease should succeed after pool creation");
    }
    test::expect_true(lease->connection().is_open(), "acquired postgres connection should be open");
}

void test_connection_pool_lease_keeps_pool_alive_until_release() {
    const database::DatabaseConfig config = test::database::build_test_database_config();
    std::optional<database::ConnectionLease> retained_lease;

    {
        auto pool = database::ConnectionPool::create(config);
        test::expect_true(pool != nullptr, "postgres pool should create before lifetime test");

        auto lease = pool->acquire_lease();
        if (!lease.has_value()) {
            test::fail("acquire_lease should succeed before releasing pool owner");
        }

        retained_lease.emplace(std::move(*lease));
        lease.reset();
    }

    test::expect_true(retained_lease.has_value(), "lease should outlive the original pool shared_ptr owner");
    retained_lease.reset();
}

void test_connection_pool_destroy_succeeds_after_replenish_requested_on_last_release() {
    const database::DatabaseConfig config = test::database::build_test_database_config();
    std::optional<database::ConnectionLease> retained_lease;

    {
        auto pool = database::ConnectionPool::create(config);
        test::expect_true(pool != nullptr, "postgres pool should create before stop during replenish test");

        auto lease = pool->acquire_lease();
        if (!lease.has_value()) {
            test::fail("acquire_lease should succeed before requesting replenish during destroy");
        }

        lease->connection().close();
        retained_lease.emplace(std::move(*lease));
        lease.reset();
    }

    test::expect_true(retained_lease.has_value(), "lease should still own the pool before final unhealthy release");
    retained_lease.reset();
}

void test_connection_pool_acquire_times_out_when_exhausted() {
    database::DatabaseConfig config = test::database::build_test_database_config();
    config.max_connections = 1;
    config.acquire_timeout_ms = 120;

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool != nullptr, "postgres pool should create with valid test database");

    auto first_lease = pool->acquire_lease();
    if (!first_lease.has_value()) {
        test::fail("first acquire_lease should succeed");
    }

    const auto start = std::chrono::steady_clock::now();
    const auto second_lease = pool->acquire_lease();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    test::expect_true(!second_lease.has_value(), "second acquire_lease should timeout when exhausted");
    test::expect_true(elapsed.count() >= 80, "acquire timeout should wait close to configured timeout");
    test::expect_true(elapsed.count() < 3000, "acquire timeout should return in bounded time");

    first_lease.reset();

    const auto third_lease = pool->acquire_lease();
    test::expect_true(third_lease.has_value(), "acquire_lease should succeed after release");
}

void test_connection_pool_replenishes_after_unhealthy_connection_discarded_with_short_acquire_timeout() {
    database::DatabaseConfig config = test::database::build_test_database_config();
    config.max_connections = 1;
    config.acquire_timeout_ms = 200;

    const auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool != nullptr, "postgres pool should create for unhealthy connection replenish test");

    auto first_lease = pool->acquire_lease();
    if (!first_lease.has_value()) {
        test::fail("first acquire_lease should succeed before discard");
    }

    first_lease->connection().close();
    first_lease.reset();

    bool recovered = false;
    const auto recover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < recover_deadline) {
        auto lease = pool->acquire_lease();
        if (lease.has_value()) {
            test::expect_true(lease->connection().is_open(), "replenished lease connection should be open");
            recovered = true;
            break;
        }
    }

    test::expect_true(recovered, "acquire_lease should recover after unhealthy discard");
}

int run_connection_pool_tests() {
    test::database::require_database_test_env();

    const std::vector<nebula::test::TestCase> tests = {
        {"postgres pool create fails when max connections invalid",
         test_connection_pool_create_fails_when_max_connections_invalid},
        {"postgres pool create fails when connect timeout invalid",
         test_connection_pool_create_fails_when_connect_timeout_invalid},
        {"postgres pool create fails when connect timeout exceeds config limit",
         test_connection_pool_create_fails_when_connect_timeout_exceeds_config_limit},
        {"postgres pool create fails when acquire timeout invalid",
         test_connection_pool_create_fails_when_acquire_timeout_invalid},
        {"postgres pool create fails when acquire timeout exceeds config limit",
         test_connection_pool_create_fails_when_acquire_timeout_exceeds_config_limit},
        {"postgres pool create fails when max connections exceeds config limit",
         test_connection_pool_create_fails_when_max_connections_exceeds_config_limit},
        {"postgres pool create fails when connection unreachable",
         test_connection_pool_create_fails_when_connection_unreachable},
        {"postgres pool create from database config succeeds with valid test env",
         test_connection_pool_create_from_database_config_succeeds_with_valid_test_env},
        {"postgres pool acquire returns open connection after create",
         test_connection_pool_acquire_returns_open_connection_after_create},
        {"postgres pool lease keeps pool alive until release",
         test_connection_pool_lease_keeps_pool_alive_until_release},
        {"postgres pool destroy succeeds after replenish requested on last release",
         test_connection_pool_destroy_succeeds_after_replenish_requested_on_last_release},
        {"postgres pool acquire times out when exhausted", test_connection_pool_acquire_times_out_when_exhausted},
        {"postgres pool replenishes after unhealthy connection discarded with short acquire timeout",
         test_connection_pool_replenishes_after_unhealthy_connection_discarded_with_short_acquire_timeout},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_connection_pool_tests);
}
