#include "nebula/database/config.hpp"

#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_database_config_validate_rejects_empty_host() {
    const database::DatabaseConfig config{
        .host = "",
        .port = 5432,
        .name = "nebula",
        .user = "nebula",
        .password = "secret",
        .max_connections = 4,
        .connect_timeout_s = 3,
        .acquire_timeout_ms = 3000,
    };

    test::expect_true(!config.validate(), "database config should reject empty host");
}

void test_database_config_validate_rejects_empty_password() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "nebula",
        .user = "nebula",
        .password = "",
        .max_connections = 4,
        .connect_timeout_s = 3,
        .acquire_timeout_ms = 3000,
    };

    std::string stderr_output;
    stderr_output = test::capture_stderr(
        [&]() { test::expect_true(!config.validate(), "database config should reject empty password"); },
        "database-config-test");
    test::expect_contains(stderr_output, "database config value invalid", "empty password should be logged");
    test::expect_contains(stderr_output, "key=\"password\"", "empty password should identify password key");
    test::expect_contains(stderr_output, "error=\"empty_value\"", "empty password should use stable error");
}

void test_database_config_validate_accepts_complete_config() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 5432,
        .name = "nebula",
        .user = "nebula",
        .password = "secret",
        .max_connections = 4,
        .connect_timeout_s = 3,
        .acquire_timeout_ms = 3000,
    };

    test::expect_true(config.validate(), "database config should accept complete values");
}

int run_config_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"database config validate rejects empty host", test_database_config_validate_rejects_empty_host},
        {"database config validate rejects empty password", test_database_config_validate_rejects_empty_password},
        {"database config validate accepts complete config", test_database_config_validate_accepts_complete_config},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_config_tests);
}
