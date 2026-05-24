#include "nebula/app/startup.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

app::StartupContext startup_with_args(const std::vector<std::string>& args) {
    test::ArgvBuilder argv(args);
    return app::startup(argv.span());
}

void test_default_startup_uses_default_config_without_file_loading() {
    const app::StartupContext startup_context = startup_with_args({"nebula"});
    test::expect_true(startup_context.ok, "default startup should succeed");
    test::expect_equal(startup_context.config_source, app::AppConfigSource::Default,
                       "default startup should use default source");
    test::expect_true(startup_context.config_path.empty(), "default startup should not carry config path");

    const app::AppConfig defaults;
    test::expect_equal(startup_context.config.server.port, defaults.server.port,
                       "default startup should keep default port");
    test::expect_equal(startup_context.config.server.backlog, defaults.server.backlog,
                       "default startup should keep default backlog");
}

void test_default_startup_reads_default_database_password_env() {
    const test::ScopedEnvVar database_password("NEBULA_DATABASE_PASSWORD", "default_startup_password");
    const app::StartupContext startup_context = startup_with_args({"nebula"});
    test::expect_true(startup_context.ok, "default startup with default database password env should succeed");
    test::expect_equal(startup_context.config.database.password, std::string("default_startup_password"),
                       "default startup should read default database password env");
}

void test_default_startup_fails_when_default_database_password_env_missing() {
    const char* original_database_password = ::getenv("NEBULA_DATABASE_PASSWORD");
    const std::string original_database_password_value =
        original_database_password == nullptr ? std::string() : std::string(original_database_password);
    ::unsetenv("NEBULA_DATABASE_PASSWORD");

    app::StartupContext startup_context;
    const std::string stderr_output =
        test::capture_stderr([&]() { startup_context = startup_with_args({"nebula"}); }, "nebula-startup-test");
    if (original_database_password == nullptr) {
        ::unsetenv("NEBULA_DATABASE_PASSWORD");
    } else {
        ::setenv("NEBULA_DATABASE_PASSWORD", original_database_password_value.c_str(), 1);
    }
    test::expect_true(!startup_context.ok, "default startup should fail when default database password env is missing");
    test::expect_contains(stderr_output, "database password resolve failed",
                          "default startup should log database password resolution failure");
    test::expect_contains(stderr_output, "env_name=\"NEBULA_DATABASE_PASSWORD\"",
                          "default startup should include default database password env name");
    test::expect_contains(stderr_output, "error=\"env_not_set\"",
                          "default startup should include stable resolve error");
    test::expect_contains(stderr_output, "load server config failed",
                          "default startup should fail at startup boundary");
    test::expect_contains(stderr_output, "source=\"default\"", "default startup failure should include config source");
    test::expect_contains(stderr_output, "error=\"server_config_invalid\"",
                          "default startup failure should include stable startup error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"",
                          "default startup failure should include decision");
}

void test_startup_loads_file_when_config_argument_present() {
    const test::TempDir dir("nebula-startup-config");
    const std::filesystem::path config_file = dir.path() / "custom.toml";
    test::write_file(config_file,
                     "[server]\n"
                     "port = 9091\n"
                     "backlog = 128\n");

    const app::StartupContext startup_context = startup_with_args({"nebula", "--config", config_file.generic_string()});
    test::expect_true(startup_context.ok, "explicit config startup should succeed");
    test::expect_equal(startup_context.config_source, app::AppConfigSource::File,
                       "explicit config startup should use file source");
    test::expect_equal(startup_context.config_path, config_file, "explicit config startup should preserve config path");
    test::expect_equal(startup_context.config.server.port, std::uint16_t{9091},
                       "explicit config startup should load port");
    test::expect_equal(startup_context.config.server.backlog, 128, "explicit config startup should load backlog");
}

void test_startup_fails_when_required_config_missing() {
    const test::TempDir dir("nebula-startup-missing-config");
    const std::filesystem::path missing = dir.path() / "missing.toml";

    app::StartupContext startup_context;
    const std::string stderr_output = test::capture_stderr(
        [&]() { startup_context = startup_with_args({"nebula", "--config", missing.generic_string()}); },
        "nebula-startup-test");
    test::expect_true(!startup_context.ok, "missing required config should fail startup");
    test::expect_equal(startup_context.config_path, missing, "missing required config should keep requested path");
    test::expect_contains(stderr_output, "app config file not found", "startup should log missing config");
    test::expect_contains(stderr_output, "load server config failed", "startup should log startup boundary failure");
    test::expect_contains(stderr_output, "error=\"server_config_invalid\"",
                          "startup boundary failure should include stable error");
    test::expect_contains(stderr_output, "error=\"config_file_not_found\"",
                          "startup failure should include stable error");
    test::expect_contains(stderr_output, "decision=\"exit_process\"", "startup failure should include decision");
}

int run_startup_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"default startup uses default config without file loading",
         test_default_startup_uses_default_config_without_file_loading},
        {"default startup reads default database password env",
         test_default_startup_reads_default_database_password_env},
        {"default startup fails when default database password env missing",
         test_default_startup_fails_when_default_database_password_env_missing},
        {"startup loads file when config argument present", test_startup_loads_file_when_config_argument_present},
        {"startup fails when required config missing", test_startup_fails_when_required_config_missing},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_startup_tests);
}
