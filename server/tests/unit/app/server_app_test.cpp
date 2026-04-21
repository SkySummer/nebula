#include "nebula/app/server_app.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::testsupport::ArgvBuilder;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::set_owner_read_write_only;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_file;

int run_server_app_with_stderr(const std::vector<std::string>& args, std::string& stderr_output) {
    ArgvBuilder argv(args);

    int exit_code = -1;
    stderr_output = nebula::testsupport::capture_stderr(
        [&]() {
            nebula::app::ServerApp app(argv.span());
            exit_code = app.run();
        },
        "nebula-server-app-test");

    return exit_code;
}

void test_run_rejected_when_main_option_invalid() {
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config"}, stderr_output);

    expect_equal(exit_code, 1, "invalid options should return non-zero");
    expect_contains(stderr_output, "parse main options failed", "should print startup parse failure");
    expect_contains(stderr_output, "decision=exit_process", "startup parse failure should include decision");
}

void test_run_rejected_when_required_config_missing() {
    const TempDir dir("nebula-server-app-missing-config");
    const auto missing_config = dir.path() / "missing.toml";

    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", missing_config.string()}, stderr_output);

    expect_equal(exit_code, 1, "missing required config should return non-zero");
    expect_contains(stderr_output, "load server config failed", "should print config load failure");
    expect_contains(stderr_output, "decision=exit_process", "config load failure should include decision");
}

void test_run_rejected_when_root_default_source_missing_reports_error() {
    const TempDir dir("nebula-server-app-root-default-missing-source");
    const auto config_file = dir.path() / "config.toml";
    write_file(config_file,
               "[routes]\n"
               "enable_healthz = false\n"
               "enable_root_default = true\n"
               "root_default_path = \"/healthz\"\n");

    ::setenv("NEBULA_DATABASE_PASSWORD", "server_app_test_password", 1);
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", config_file.string()}, stderr_output);
    ::unsetenv("NEBULA_DATABASE_PASSWORD");

    expect_equal(exit_code, 1, "missing root default source route should return non-zero");
    expect_contains(stderr_output, "register default route failed",
                    "root default registration failure should be logged");
    expect_contains(stderr_output, "error=source_get_route_not_found",
                    "root default registration failure should include error");
    expect_contains(stderr_output, "decision=exit_process",
                    "root default registration failure should include decision");
}

void test_run_rejected_when_database_unreachable() {
    const TempDir dir("nebula-server-app-database-unreachable");
    const auto config_file = dir.path() / "config.toml";
    write_file(config_file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "jwt_secret_path = \"runtime/secrets/jwt.key\"\n"
               "\n"
               "[database]\n"
               "host = \"127.0.0.1\"\n"
               "port = 1\n"
               "name = \"invalid_db\"\n"
               "user = \"invalid_user\"\n"
               "password_env = \"NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD\"\n"
               "max_connections = 1\n"
               "connect_timeout_ms = 500\n");

    ::setenv("NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD", "invalid_password", 1);
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", config_file.string()}, stderr_output);
    ::unsetenv("NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD");

    expect_equal(exit_code, 1, "unreachable database should return non-zero");
    expect_contains(stderr_output, "database pool init failed", "database pool init failure should be logged");
    expect_contains(stderr_output, "decision=exit_process", "failure should include decision");
}

void test_run_rejected_when_jwt_secret_encoding_invalid() {
    const TempDir dir("nebula-server-app-invalid-jwt-secret");
    const auto config_file = dir.path() / "config.toml";
    const auto secret_file = dir.path() / "jwt.key";
    write_file(secret_file, "not-valid-base64!!!");
    set_owner_read_write_only(secret_file);
    write_file(config_file, std::format("[routes]\n"
                                        "enable_root_default = false\n"
                                        "\n"
                                        "[auth]\n"
                                        "jwt_secret_path = \"{}\"\n",
                                        secret_file.string()));

    ::setenv("NEBULA_DATABASE_PASSWORD", "server_app_test_password", 1);
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", config_file.string()}, stderr_output);
    ::unsetenv("NEBULA_DATABASE_PASSWORD");

    expect_equal(exit_code, 1, "invalid jwt secret encoding should return non-zero");
    expect_contains(stderr_output, "load jwt secret failed", "jwt secret decode failure should be logged");
    expect_contains(stderr_output, "error=invalid_secret_encoding",
                    "jwt secret decode failure should include stable error code");
    expect_contains(stderr_output, "register auth routes failed",
                    "server should report auth route registration failure");
    expect_contains(stderr_output, "decision=exit_process", "failure should include decision");
}

int run_server_app_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"run rejected when main option invalid", test_run_rejected_when_main_option_invalid},
        {"run rejected when required config missing", test_run_rejected_when_required_config_missing},
        {"run rejected when root default source missing reports error",
         test_run_rejected_when_root_default_source_missing_reports_error},
        {"run rejected when database unreachable", test_run_rejected_when_database_unreachable},
        {"run rejected when jwt secret encoding invalid", test_run_rejected_when_jwt_secret_encoding_invalid},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_app_tests);
}
