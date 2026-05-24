#include "nebula/app/server_app.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void write_server_app_config(const std::filesystem::path& config_file, const std::filesystem::path& log_dir,
                             std::string_view content) {
    std::string config_text(content);
    if (!config_text.empty() && config_text.back() != '\n') {
        config_text.push_back('\n');
    }
    config_text.append(
        std::format("[logger]\n"
                    "dir = \"{}\"\n"
                    "also_stderr = true\n",
                    log_dir.generic_string()));
    test::write_file(config_file, config_text);
}

int run_server_app_with_stderr(const std::vector<std::string>& args, std::string& stderr_output) {
    test::ArgvBuilder argv(args);

    int exit_code = -1;
    stderr_output = nebula::test::capture_stderr(
        [&]() {
            nebula::app::ServerApp app(argv.span());
            exit_code = app.run();
        },
        "nebula-server-app-test");

    return exit_code;
}

struct RunTwiceResult {
    int first_exit_code = -1;
    int second_exit_code = -1;
    std::string stderr_output;
};

RunTwiceResult run_server_app_twice_with_stderr(const std::vector<std::string>& args) {
    test::ArgvBuilder argv(args);

    RunTwiceResult result;
    result.stderr_output = nebula::test::capture_stderr(
        [&]() {
            nebula::app::ServerApp app(argv.span());
            result.first_exit_code = app.run();
            result.second_exit_code = app.run();
        },
        "nebula-server-app-run-twice-test");

    return result;
}

void test_run_rejected_when_main_option_invalid() {
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config"}, stderr_output);

    test::expect_equal(exit_code, 1, "invalid options should return non-zero");
    test::expect_contains(stderr_output, "parse main options failed", "should print startup parse failure");
    test::expect_contains(stderr_output, "error=\"main_options_invalid\"",
                          "startup parse failure should include boundary error");
    test::expect_contains(stderr_output, "error=\"missing_value\"",
                          "startup parse failure should include detailed option error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"", "startup parse failure should include decision");
}

void test_run_rejected_when_required_config_missing() {
    const test::TempDir dir("nebula-server-app-missing-config");
    const auto missing_config = dir.path() / "missing.toml";

    std::string stderr_output;
    const int exit_code =
        run_server_app_with_stderr({"nebula", "--config", missing_config.generic_string()}, stderr_output);

    test::expect_equal(exit_code, 1, "missing required config should return non-zero");
    test::expect_contains(stderr_output, "app config file not found", "should print config load failure");
    test::expect_contains(stderr_output, "load server config failed", "should print startup boundary failure");
    test::expect_contains(stderr_output, "error=\"server_config_invalid\"",
                          "config load boundary failure should include stable error");
    test::expect_contains(stderr_output, "error=\"config_file_not_found\"",
                          "config load failure should include stable error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"", "config load failure should include decision");
}

void test_run_rejected_when_root_default_source_missing_reports_error() {
    const test::TempDir dir("nebula-server-app-root-default-missing-source");
    const auto config_file = dir.path() / "config.toml";
    write_server_app_config(config_file, dir.path() / "logs",
                            "[routes]\n"
                            "enable_healthz = false\n"
                            "enable_root_default = true\n"
                            "root_default_path = \"/healthz\"\n");

    std::string stderr_output;
    const int exit_code =
        run_server_app_with_stderr({"nebula", "--config", config_file.generic_string()}, stderr_output);

    test::expect_equal(exit_code, 1, "missing root default source route should return non-zero");
    test::expect_contains(stderr_output, "register default route failed",
                          "root default registration failure should be logged");
    test::expect_contains(stderr_output, "error=\"source_get_route_not_found\"",
                          "root default registration failure should include error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"",
                          "root default registration failure should include decision");
}

void test_run_rejected_when_database_unreachable() {
    const test::TempDir dir("nebula-server-app-database-unreachable");
    const auto config_file = dir.path() / "config.toml";
    write_server_app_config(config_file, dir.path() / "logs",
                            "[routes]\n"
                            "enable_root_default = false\n"
                            "\n"
                            "[database]\n"
                            "host = \"127.0.0.1\"\n"
                            "port = 1\n"
                            "name = \"invalid_db\"\n"
                            "user = \"invalid_user\"\n"
                            "password_env = \"NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD\"\n"
                            "max_connections = 1\n"
                            "connect_timeout_s = 1\n"
                            "\n"
                            "[auth]\n"
                            "jwt_secret_path = \"runtime/secrets/jwt.key\"\n");

    ::setenv("NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD", "invalid_password", 1);
    std::string stderr_output;
    const int exit_code =
        run_server_app_with_stderr({"nebula", "--config", config_file.generic_string()}, stderr_output);
    ::unsetenv("NEBULA_TEST_SERVER_APP_INVALID_DB_PASSWORD");

    test::expect_equal(exit_code, 1, "unreachable database should return non-zero");
    test::expect_contains(stderr_output, "database pool init failed", "database pool init failure should be logged");
    test::expect_contains(stderr_output, "decision=\"exit_process\"", "failure should include decision");
}

void test_run_rejected_when_database_password_env_missing() {
    const test::TempDir dir("nebula-server-app-database-password-env-missing");
    const auto config_file = dir.path() / "config.toml";
    write_server_app_config(config_file, dir.path() / "logs",
                            "[routes]\n"
                            "enable_root_default = false\n"
                            "\n"
                            "[database]\n"
                            "host = \"127.0.0.1\"\n"
                            "port = 5432\n"
                            "name = \"nebula\"\n"
                            "user = \"nebula\"\n"
                            "password_env = \"NEBULA_TEST_SERVER_APP_MISSING_DB_PASSWORD\"\n"
                            "max_connections = 1\n"
                            "connect_timeout_s = 1\n"
                            "\n"
                            "[auth]\n"
                            "jwt_secret_path = \"runtime/secrets/jwt.key\"\n");

    ::unsetenv("NEBULA_TEST_SERVER_APP_MISSING_DB_PASSWORD");
    std::string stderr_output;
    const int exit_code =
        run_server_app_with_stderr({"nebula", "--config", config_file.generic_string()}, stderr_output);

    test::expect_equal(exit_code, 1, "missing database password env should return non-zero");
    test::expect_contains(stderr_output, "database password resolve failed",
                          "missing database password env should fail during config load");
    test::expect_contains(stderr_output, "env_name=\"NEBULA_TEST_SERVER_APP_MISSING_DB_PASSWORD\"",
                          "missing database password env should include configured env key");
    test::expect_contains(stderr_output, "error=\"env_not_set\"",
                          "missing database password env should include resolve error");
    test::expect_contains(stderr_output, "key=\"password\"",
                          "missing database password env should invalidate database password");
    test::expect_contains(stderr_output, "load server config failed",
                          "missing database password env should fail startup");
    test::expect_contains(stderr_output, "error=\"server_config_invalid\"",
                          "missing database password env should include startup boundary error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"", "failure should include decision");
}

void test_run_rejected_when_database_init_precedes_jwt_secret_validation() {
    const test::TempDir dir("nebula-server-app-invalid-jwt-secret");
    const auto config_file = dir.path() / "config.toml";
    const auto secret_file = dir.path() / "jwt.key";
    test::write_file(secret_file, "not-valid-base64!!!");
    test::set_owner_read_write_only(secret_file);
    ::unsetenv("NEBULA_TEST_SERVER_APP_JWT_ORDER_DB_PASSWORD");
    write_server_app_config(config_file, dir.path() / "logs",
                            std::format("[routes]\n"
                                        "enable_root_default = false\n"
                                        "\n"
                                        "[database]\n"
                                        "password_env = \"NEBULA_TEST_SERVER_APP_JWT_ORDER_DB_PASSWORD\"\n"
                                        "\n"
                                        "[auth]\n"
                                        "jwt_secret_path = \"{}\"\n",
                                        secret_file.generic_string()));

    std::string stderr_output;
    const int exit_code =
        run_server_app_with_stderr({"nebula", "--config", config_file.generic_string()}, stderr_output);

    test::expect_equal(exit_code, 1, "database init failure should return non-zero before jwt validation");
    test::expect_contains(stderr_output, "database password resolve failed",
                          "missing explicit database password env should be logged before startup fails");
    test::expect_contains(stderr_output, "env_name=\"NEBULA_TEST_SERVER_APP_JWT_ORDER_DB_PASSWORD\"",
                          "missing explicit database password env should include configured env key");
    test::expect_contains(stderr_output, "error=\"env_not_set\"",
                          "missing explicit database password env should include error");
    test::expect_contains(stderr_output, "key=\"password\"",
                          "missing explicit database password env should invalidate password");
    test::expect_contains(stderr_output, "load server config failed",
                          "database config failure should stop startup before auth module jwt validation");
    test::expect_contains(stderr_output, "error=\"server_config_invalid\"",
                          "database config failure should include startup boundary error code");
    test::expect_not_contains(stderr_output, "load jwt secret failed",
                              "jwt secret validation should not run before database init succeeds");
}

void test_run_rejected_when_called_more_than_once() {
    const RunTwiceResult result = run_server_app_twice_with_stderr({"nebula", "--config"});

    test::expect_equal(result.first_exit_code, 1, "first run with invalid options should return non-zero");
    test::expect_equal(result.second_exit_code, 1, "second run should be rejected");
    test::expect_contains(result.stderr_output, "server app run rejected",
                          "second run should log repeated run rejection");
    test::expect_contains(result.stderr_output, "error=\"already_started\"",
                          "repeated run rejection should include stable error code");
}

int run_server_app_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"run rejected when main option invalid", test_run_rejected_when_main_option_invalid},
        {"run rejected when required config missing", test_run_rejected_when_required_config_missing},
        {"run rejected when root default source missing reports error",
         test_run_rejected_when_root_default_source_missing_reports_error},
        {"run rejected when database unreachable", test_run_rejected_when_database_unreachable},
        {"run rejected when database password env missing", test_run_rejected_when_database_password_env_missing},
        {"run rejected when database init precedes jwt secret validation",
         test_run_rejected_when_database_init_precedes_jwt_secret_validation},
        {"run rejected when called more than once", test_run_rejected_when_called_more_than_once},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_server_app_tests);
}
