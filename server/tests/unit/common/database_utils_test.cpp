#include "nebula/common/database_utils.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::resolve_database_password;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::fail;

void test_resolve_database_password_rejects_empty_env_name() {
    const std::optional<std::string> password = resolve_database_password("");
    expect_true(!password.has_value(), "empty env name should be rejected");
}

void test_resolve_database_password_returns_nullopt_when_env_missing() {
    ::unsetenv("NEBULA_TEST_DB_PASSWORD_MISSING");
    const std::optional<std::string> password = resolve_database_password("NEBULA_TEST_DB_PASSWORD_MISSING");
    expect_true(!password.has_value(), "missing env value should return nullopt");
}

void test_resolve_database_password_returns_nullopt_when_env_empty() {
    ::setenv("NEBULA_TEST_DB_PASSWORD_EMPTY", "", 1);
    const std::optional<std::string> password = resolve_database_password("NEBULA_TEST_DB_PASSWORD_EMPTY");
    expect_true(!password.has_value(), "empty env value should return nullopt");
}

void test_resolve_database_password_returns_env_value() {
    ::setenv("NEBULA_TEST_DB_PASSWORD_SET", "nebula_password_from_env", 1);
    const std::optional<std::string> password = resolve_database_password("NEBULA_TEST_DB_PASSWORD_SET");
    expect_true(password.has_value(), "existing env should resolve");
    if (!password.has_value()) {
        fail("existing env should resolve");
    }
    expect_equal(password.value(), std::string("nebula_password_from_env"),
                 "resolved env should match configured value");
}

int run_database_utils_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"resolve database password rejects empty env name", test_resolve_database_password_rejects_empty_env_name},
        {"resolve database password returns nullopt when env missing",
         test_resolve_database_password_returns_nullopt_when_env_missing},
        {"resolve database password returns nullopt when env empty",
         test_resolve_database_password_returns_nullopt_when_env_empty},
        {"resolve database password returns env value", test_resolve_database_password_returns_env_value},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_database_utils_tests);
}
