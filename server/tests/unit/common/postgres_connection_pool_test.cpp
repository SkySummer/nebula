#include "nebula/common/postgres_connection_pool.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nebula_tests/test_support.hpp"
#include "nebula_tests/test_support_database.hpp"

namespace {

using nebula::common::PostgresConnectionPool;
using nebula::common::PostgresConnectionPoolOptions;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::database::require_postgres_pool_test_options;
using nebula::testsupport::database::validate_database_test_env;

void test_connection_pool_status_to_string_contract() {
    expect_equal(nebula::common::to_string(PostgresConnectionPool::InitializeStatus::Initialized),
                 std::string_view("initialized"), "initialize status initialized should map to initialized");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::InitializeStatus::AlreadyInitialized),
                 std::string_view("already_initialized"),
                 "initialize status already initialized should map to already_initialized");
    expect_equal(
        nebula::common::to_string(PostgresConnectionPool::InitializeStatus::AlreadyInitializedWithDifferentOptions),
        std::string_view("already_initialized_with_different_options"),
        "initialize status already initialized with different options should map to expected text");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::InitializeStatus::InvalidConfig),
                 std::string_view("invalid_config"), "initialize status invalid config should map to invalid_config");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::InitializeStatus::ConnectionCreateFailed),
                 std::string_view("connection_create_failed"),
                 "initialize status connection create failed should map to connection_create_failed");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::InitializeStatus::ReplenishWorkerStartFailed),
                 std::string_view("replenish_worker_start_failed"),
                 "initialize status replenish worker start failed should map to replenish_worker_start_failed");

    expect_equal(nebula::common::to_string(PostgresConnectionPool::AcquireStatus::Acquired),
                 std::string_view("acquired"), "acquire status acquired should map to acquired");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::AcquireStatus::TimedOut),
                 std::string_view("timed_out"), "acquire status timed out should map to timed_out");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::AcquireStatus::PoolNotInitialized),
                 std::string_view("pool_not_initialized"),
                 "acquire status pool not initialized should map to pool_not_initialized");
    expect_equal(nebula::common::to_string(PostgresConnectionPool::AcquireStatus::PoolStopping),
                 std::string_view("pool_stopping"), "acquire status pool stopping should map to pool_stopping");
}

void test_connection_pool_initialize_fails_when_max_connections_invalid() {
    PostgresConnectionPool pool;

    PostgresConnectionPoolOptions options{
        .host = "127.0.0.1",
        .port = 5432,
        .database = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 0,
        .connect_timeout_ms = 1000,
        .acquire_timeout_ms = 1000,
    };

    const PostgresConnectionPool::InitializeStatus status = pool.initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::InvalidConfig,
                 "postgres pool should fail when max_connections is invalid");
    expect_true(!pool.is_initialized(), "postgres pool should remain unavailable");
}

void test_connection_pool_initialize_fails_when_connect_timeout_invalid() {
    PostgresConnectionPool pool;

    PostgresConnectionPoolOptions options{
        .host = "127.0.0.1",
        .port = 5432,
        .database = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_ms = 0,
        .acquire_timeout_ms = 1000,
    };

    const PostgresConnectionPool::InitializeStatus status = pool.initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::InvalidConfig,
                 "postgres pool should fail when connect timeout is invalid");
    expect_true(!pool.is_initialized(), "postgres pool should remain unavailable");
}

void test_connection_pool_initialize_fails_when_acquire_timeout_invalid() {
    PostgresConnectionPool pool;

    PostgresConnectionPoolOptions options{
        .host = "127.0.0.1",
        .port = 5432,
        .database = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_ms = 1000,
        .acquire_timeout_ms = 0,
    };

    const PostgresConnectionPool::InitializeStatus status = pool.initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::InvalidConfig,
                 "postgres pool should fail when acquire timeout is invalid");
    expect_true(!pool.is_initialized(), "postgres pool should remain unavailable");
}

void test_connection_pool_initialize_fails_when_connection_unreachable() {
    PostgresConnectionPool pool;

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

    const PostgresConnectionPool::InitializeStatus status = pool.initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::ConnectionCreateFailed,
                 "postgres pool should fail initialize when database is unreachable");
    expect_true(!pool.is_initialized(), "postgres pool should remain unavailable when database is unreachable");
}

void test_connection_pool_initialize_rejects_different_options_after_initialized() {
    const PostgresConnectionPoolOptions base_options = require_postgres_pool_test_options();

    PostgresConnectionPool pool;

    const PostgresConnectionPool::InitializeStatus first_status = pool.initialize(base_options);
    expect_equal(first_status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize with valid test database");

    const PostgresConnectionPool::InitializeStatus same_options_status = pool.initialize(base_options);
    expect_equal(same_options_status, PostgresConnectionPool::InitializeStatus::AlreadyInitialized,
                 "postgres pool should report already_initialized when options are unchanged");

    PostgresConnectionPoolOptions different_options = base_options;
    ++different_options.max_connections;

    const PostgresConnectionPool::InitializeStatus different_options_status = pool.initialize(different_options);
    expect_equal(different_options_status,
                 PostgresConnectionPool::InitializeStatus::AlreadyInitializedWithDifferentOptions,
                 "postgres pool should reject reinitialize with different options");
}

void test_connection_pool_acquire_returns_open_connection_after_initialized() {
    const PostgresConnectionPoolOptions base_options = require_postgres_pool_test_options();
    PostgresConnectionPool pool;
    const PostgresConnectionPool::InitializeStatus init_status = pool.initialize(base_options);
    expect_equal(init_status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize with valid test database");
    expect_true(pool.is_initialized(), "postgres pool should report initialized after successful init");

    PostgresConnectionPool::AcquireResult acquire_result = pool.acquire_connection();
    expect_equal(acquire_result.status, PostgresConnectionPool::AcquireStatus::Acquired,
                 "acquire_connection should succeed after pool initialization");
    expect_true(acquire_result.lease.has_value(), "acquire_connection should provide lease after initialization");
    if (!acquire_result.lease.has_value()) {
        return;
    }
    expect_true(acquire_result.lease->connection().is_open(), "acquired postgres connection should be open");
}

void test_connection_pool_acquire_times_out_when_exhausted() {
    const PostgresConnectionPoolOptions base_options = require_postgres_pool_test_options();
    PostgresConnectionPool pool;
    PostgresConnectionPoolOptions options = base_options;
    options.max_connections = 1;
    options.acquire_timeout_ms = 120;

    const PostgresConnectionPool::InitializeStatus status = pool.initialize(options);
    expect_equal(status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize with valid test database");

    PostgresConnectionPool::AcquireResult first_acquire = pool.acquire_connection();
    expect_equal(first_acquire.status, PostgresConnectionPool::AcquireStatus::Acquired,
                 "first acquire_connection should succeed");
    expect_true(first_acquire.lease.has_value(), "first acquire_connection should provide lease");
    if (!first_acquire.lease.has_value()) {
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    const PostgresConnectionPool::AcquireResult second_acquire = pool.acquire_connection();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    expect_equal(second_acquire.status, PostgresConnectionPool::AcquireStatus::TimedOut,
                 "second acquire_connection should timeout when pool is exhausted");
    expect_true(!second_acquire.lease.has_value(), "timed out acquire should not provide lease");
    expect_true(elapsed.count() >= 80, "acquire timeout should wait close to configured timeout");
    expect_true(elapsed.count() < 3000, "acquire timeout should return in bounded time");

    first_acquire.lease.reset();

    const PostgresConnectionPool::AcquireResult third_acquire = pool.acquire_connection();
    expect_equal(third_acquire.status, PostgresConnectionPool::AcquireStatus::Acquired,
                 "acquire_connection should succeed after lease is returned");
    expect_true(third_acquire.lease.has_value(), "acquire_connection should provide lease after release");
}

void test_connection_pool_replenishes_after_unhealthy_connection_discarded_with_short_acquire_timeout() {
    const PostgresConnectionPoolOptions base_options = require_postgres_pool_test_options();
    PostgresConnectionPool pool;
    PostgresConnectionPoolOptions options = base_options;
    options.max_connections = 1;
    options.acquire_timeout_ms = 200;

    const PostgresConnectionPool::InitializeStatus init_status = pool.initialize(options);
    expect_equal(init_status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize for unhealthy connection replenish test");

    PostgresConnectionPool::AcquireResult first_acquire = pool.acquire_connection();
    expect_equal(first_acquire.status, PostgresConnectionPool::AcquireStatus::Acquired,
                 "first acquire_connection should succeed before discarding unhealthy connection");
    expect_true(first_acquire.lease.has_value(), "first acquire_connection should provide lease");
    if (!first_acquire.lease.has_value()) {
        return;
    }

    first_acquire.lease->connection().close();
    first_acquire.lease.reset();

    bool recovered = false;
    const auto recover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < recover_deadline) {
        auto acquire_result = pool.acquire_connection();
        if (acquire_result.status == PostgresConnectionPool::AcquireStatus::Acquired) {
            expect_true(acquire_result.lease.has_value(), "replenished pool should provide lease");
            if (!acquire_result.lease.has_value()) {
                return;
            }
            expect_true(acquire_result.lease->connection().is_open(), "replenished lease connection should be open");
            recovered = true;
            break;
        }
        expect_equal(acquire_result.status, PostgresConnectionPool::AcquireStatus::TimedOut,
                     "acquire_connection should either timeout or succeed while replenishing");
    }

    expect_true(recovered, "acquire_connection should recover after unhealthy connection is discarded");
}

void test_connection_pool_replenishes_after_acquire_discards_unhealthy_available_connection() {
    const PostgresConnectionPoolOptions base_options = require_postgres_pool_test_options();
    PostgresConnectionPool pool;
    PostgresConnectionPoolOptions options = base_options;
    options.max_connections = 2;
    options.acquire_timeout_ms = 200;

    const PostgresConnectionPool::InitializeStatus init_status = pool.initialize(options);
    expect_equal(init_status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should initialize for acquire discard replenish test");

    expect_equal(pool.connection_count_for_test(), options.max_connections,
                 "postgres pool should start with full capacity before closing available connection for test");

    const bool closed = pool.close_one_available_connection_for_test();
    expect_true(closed, "testing api should close one available connection");
    if (!closed) {
        return;
    }

    PostgresConnectionPool::AcquireResult acquire_result = pool.acquire_connection();
    expect_equal(acquire_result.status, PostgresConnectionPool::AcquireStatus::Acquired,
                 "acquire_connection should still succeed after discarding unhealthy available connection");
    expect_true(acquire_result.lease.has_value(), "acquire_connection should provide lease after discard");
    if (!acquire_result.lease.has_value()) {
        return;
    }
    expect_true(acquire_result.lease->connection().is_open(), "acquired lease connection should be open");
    acquire_result.lease.reset();

    bool replenished = false;
    const auto replenish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < replenish_deadline) {
        if (pool.connection_count_for_test() == options.max_connections) {
            replenished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    expect_true(replenished,
                "pool should replenish back to max connections after acquire discards unhealthy available connection");
}

void test_connection_pool_initialize_fails_when_replenish_worker_start_fails() {
    const PostgresConnectionPoolOptions options = require_postgres_pool_test_options();

    PostgresConnectionPool pool;
    pool.set_replenish_worker_start_failure_for_test(true);

    const PostgresConnectionPool::InitializeStatus failed_init_status = pool.initialize(options);
    expect_equal(failed_init_status, PostgresConnectionPool::InitializeStatus::ReplenishWorkerStartFailed,
                 "postgres pool should fail initialize when replenish worker startup fails");
    expect_true(!pool.is_initialized(), "postgres pool should remain unavailable when replenish worker startup fails");

    pool.set_replenish_worker_start_failure_for_test(false);

    const PostgresConnectionPool::InitializeStatus retry_init_status = pool.initialize(options);
    expect_equal(retry_init_status, PostgresConnectionPool::InitializeStatus::Initialized,
                 "postgres pool should recover after replenish worker startup failure is cleared");
    expect_true(pool.is_initialized(), "postgres pool should be available after replenish worker startup recovers");
}

int run_postgres_connection_pool_tests() {
    const std::optional<std::string> env_error = validate_database_test_env();
    if (env_error.has_value()) {
        std::cerr << "[SKIP] postgres connection pool test precheck skipped: error=" << *env_error << '\n';
        return nebula::testsupport::kTestSkipReturnCode;
    }

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"postgres pool status to_string contract", test_connection_pool_status_to_string_contract},
        {"postgres pool initialize fails when max connections invalid",
         test_connection_pool_initialize_fails_when_max_connections_invalid},
        {"postgres pool initialize fails when connect timeout invalid",
         test_connection_pool_initialize_fails_when_connect_timeout_invalid},
        {"postgres pool initialize fails when acquire timeout invalid",
         test_connection_pool_initialize_fails_when_acquire_timeout_invalid},
        {"postgres pool initialize fails when connection unreachable",
         test_connection_pool_initialize_fails_when_connection_unreachable},
        {"postgres pool initialize rejects different options after initialized",
         test_connection_pool_initialize_rejects_different_options_after_initialized},
        {"postgres pool initialize fails when replenish worker start fails",
         test_connection_pool_initialize_fails_when_replenish_worker_start_fails},
        {"postgres pool acquire returns open connection after initialized",
         test_connection_pool_acquire_returns_open_connection_after_initialized},
        {"postgres pool acquire times out when exhausted", test_connection_pool_acquire_times_out_when_exhausted},
        {"postgres pool replenishes after unhealthy connection discarded with short acquire timeout",
         test_connection_pool_replenishes_after_unhealthy_connection_discarded_with_short_acquire_timeout},
        {"postgres pool replenishes after acquire discards unhealthy available connection",
         test_connection_pool_replenishes_after_acquire_discards_unhealthy_available_connection},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_postgres_connection_pool_tests);
}
