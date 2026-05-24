#include "nebula/app/app_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "nebula/common/log/logger.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

struct CapturedLoadResult {
    app::AppConfigLoadResult result;
    std::string stderr_output;
};

CapturedLoadResult load_app_config_with_stderr(const std::filesystem::path& file) {
    CapturedLoadResult captured;
    const test::TempDir dir("nebula-app-config-log-reset");
    nebula::common::Logger::instance().initialize(
        {.level = nebula::common::LogLevel::Trace, .dir = dir.path(), .also_stderr = true});
    captured.stderr_output = test::capture_stderr([&]() { captured.result = nebula::app::load_app_config(file); },
                                                  "nebula-server-config-test");
    return captured;
}

void test_load_full_valid_config() {
    const test::TempDir dir("nebula-server-config-valid");
    const std::filesystem::path file = dir.path() / "server.toml";
    ::setenv("NEBULA_TEST_DATABASE_PASSWORD_CFG", "cfg_secret_value", 1);
    test::write_file(file,
                     "[server]\n"
                     "port = 9090\n"
                     "backlog = 2048\n"
                     "max_connections = 5000\n"
                     "sub_reactor_count = 3\n"
                     "worker_thread_count = 6\n"
                     "manage_signals = false\n"
                     "\n"
                     "[logger]\n"
                     "level = \"fAtAl\"\n"
                     "dir = \"runtime/custom-logs\"\n"
                     "also_stderr = false\n"
                     "\n"
                     "[timeouts]\n"
                     "read_timeout_ms = 12345\n"
                     "graceful_shutdown_timeout_ms = 2500\n"
                     "\n"
                     "[limits]\n"
                     "max_header_bytes = 4096\n"
                     "max_request_target_bytes = 2048\n"
                     "max_body_bytes = 8192\n"
                     "\n"
                     "[routes]\n"
                     "enable_healthz = false\n"
                     "enable_echo = true\n"
                     "enable_root_default = true\n"
                     "root_default_path = \"/healthz\"\n"
                     "\n"
                     "[database]\n"
                     "host = \"db.local\"\n"
                     "port = 15432\n"
                     "name = \"nebula_test\"\n"
                     "user = \"nebula_user\"\n"
                     "password_env = \"NEBULA_TEST_DATABASE_PASSWORD_CFG\"\n"
                     "max_connections = 12\n"
                     "connect_timeout_s = 3\n"
                     "acquire_timeout_ms = 3500\n"
                     "\n"
                     "[auth]\n"
                     "jwt_secret_path = \"runtime/secrets/test_jwt.key\"\n"
                     "access_token_ttl_s = 7200\n"
                     "password_hash_iterations = 240000\n"
                     "\n"
                     "[storage]\n"
                     "root_dir = \"runtime/custom-files\"\n"
                     "upload_session_ttl_s = 7200\n"
                     "download_ticket_ttl_s = 300\n"
                     "max_file_kb = 120\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    ::unsetenv("NEBULA_TEST_DATABASE_PASSWORD_CFG");
    test::expect_true(captured.result.ok, "valid config should load");
    test::expect_equal(captured.result.source, app::AppConfigSource::File, "valid config should report file source");

    const app::AppConfig& config = captured.result.config;
    test::expect_equal(config.server.port, std::uint16_t{9090}, "port should map");
    test::expect_equal(config.server.backlog, 2048, "backlog should map");
    test::expect_equal(config.server.max_connections, std::size_t{5000}, "max_connections should map");
    test::expect_equal(config.server.sub_reactor_count, std::size_t{3}, "sub_reactor_count should map");
    test::expect_equal(config.server.worker_thread_count, std::size_t{6}, "worker_thread_count should map");
    test::expect_true(!config.server.manage_signals, "manage_signals should map");
    test::expect_equal(config.logger.level, nebula::common::LogLevel::Fatal, "log level should parse case-insensitive");
    test::expect_equal(config.logger.dir.generic_string(), std::string("runtime/custom-logs"), "log_dir should map");
    test::expect_true(!config.logger.also_stderr, "also_stderr should map");
    test::expect_equal(config.timeouts.read_timeout.count(), std::int64_t{12345}, "read timeout should map");
    test::expect_equal(config.timeouts.graceful_shutdown_timeout.count(), std::int64_t{2500},
                       "graceful shutdown timeout should map");
    test::expect_equal(config.limits.max_header_bytes, std::size_t{4096}, "max_header_bytes should map");
    test::expect_equal(config.limits.max_request_target_bytes, std::size_t{2048},
                       "max_request_target_bytes should map");
    test::expect_equal(config.limits.max_body_bytes, std::size_t{8192}, "max_body_bytes should map");
    test::expect_true(!config.routes.enable_healthz, "enable_healthz should map");
    test::expect_true(config.routes.enable_echo, "enable_echo should map");
    test::expect_true(config.routes.enable_root_default, "enable_root_default should map");
    test::expect_equal(config.routes.root_default_path, std::string("/healthz"), "root_default_path should map");
    test::expect_equal(config.auth.jwt_secret_path.generic_string(), std::string("runtime/secrets/test_jwt.key"),
                       "auth jwt secret path should map");
    test::expect_equal(config.auth.access_token_ttl_s, std::int64_t{7200}, "auth ttl should map");
    test::expect_equal(config.auth.password_hash_iterations, std::uint32_t{240000}, "auth hash iterations should map");
    test::expect_equal(config.database.host, std::string("db.local"), "database host should map");
    test::expect_equal(config.database.port, std::uint16_t{15432}, "database port should map");
    test::expect_equal(config.database.name, std::string("nebula_test"), "database name should map");
    test::expect_equal(config.database.user, std::string("nebula_user"), "database user should map");
    test::expect_equal(config.database.password, std::string("cfg_secret_value"), "database password should resolve");
    test::expect_equal(config.database.max_connections, std::size_t{12}, "database max connections should map");
    test::expect_equal(config.database.connect_timeout_s, std::int64_t{3}, "database connect timeout should map");
    test::expect_equal(config.database.acquire_timeout_ms, std::int64_t{3500}, "database acquire timeout should map");
    test::expect_equal(config.storage.root_dir.generic_string(), std::string("runtime/custom-files"),
                       "storage root should map");
    test::expect_equal(config.storage.upload_session_ttl_s, std::int64_t{7200},
                       "storage upload session ttl should map");
    test::expect_equal(config.storage.download_ticket_ttl_s, std::int64_t{300},
                       "storage download ticket ttl should map");
    test::expect_equal(config.storage.max_file_bytes, static_cast<std::int64_t>(120 * 1024),
                       "storage max file bytes should map");
}

void test_max_connections_zero_rejected() {
    const test::TempDir dir("nebula-server-config-max-connections-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "max_connections = 0\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "max_connections zero should fail");
    test::expect_contains(captured.stderr_output, "server config value out of range",
                          "max_connections zero should be logged");
    test::expect_contains(captured.stderr_output, "key=\"max_connections\"", "max_connections zero should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "max_connections zero should be logged");
}

void test_worker_thread_count_zero_maps_to_default_auto_value() {
    const test::TempDir dir("nebula-server-config-worker-threads-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "worker_thread_count = 0\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok, "worker_thread_count zero should load");
    test::expect_equal(captured.result.source, app::AppConfigSource::File,
                       "worker_thread_count zero should keep file source");
    test::expect_equal(captured.result.config.server.worker_thread_count, nebula::server::default_worker_thread_count(),
                       "worker_thread_count zero should map to default auto value");
}

void test_sub_reactor_count_zero_maps_to_default_auto_value() {
    const test::TempDir dir("nebula-server-config-io-threads-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "sub_reactor_count = 0\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok, "sub_reactor_count zero should load");
    test::expect_equal(captured.result.source, app::AppConfigSource::File,
                       "sub_reactor_count zero should keep file source");
    test::expect_equal(captured.result.config.server.sub_reactor_count, nebula::server::default_sub_reactor_count(),
                       "sub_reactor_count zero should map to default auto value");
}

void test_max_connections_above_limit_rejected() {
    const test::TempDir dir("nebula-server-config-max-connections-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "max_connections = 1000001\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "too large server.max_connections should fail");
    test::expect_contains(captured.stderr_output, "server config value out of range",
                          "too large server.max_connections should be logged");
    test::expect_contains(captured.stderr_output, "key=\"max_connections\"",
                          "too large server.max_connections should be logged");
    test::expect_contains(captured.stderr_output, "value=1000001", "too large server.max_connections should be logged");
}

void test_sub_reactor_count_above_limit_rejected() {
    const test::TempDir dir("nebula-server-config-sub-reactor-count-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "sub_reactor_count = 65\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "too large server.sub_reactor_count should fail");
    test::expect_contains(captured.stderr_output, "server config value out of range",
                          "too large server.sub_reactor_count should be logged");
    test::expect_contains(captured.stderr_output, "key=\"sub_reactor_count\"",
                          "too large server.sub_reactor_count should be logged");
    test::expect_contains(captured.stderr_output, "value=65", "too large server.sub_reactor_count should be logged");
}

void test_worker_thread_count_above_limit_rejected() {
    const test::TempDir dir("nebula-server-config-worker-thread-count-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "worker_thread_count = 257\n"
                     "\n"
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "too large server.worker_thread_count should fail");
    test::expect_contains(captured.stderr_output, "server config value out of range",
                          "too large server.worker_thread_count should be logged");
    test::expect_contains(captured.stderr_output, "key=\"worker_thread_count\"",
                          "too large server.worker_thread_count should be logged");
    test::expect_contains(captured.stderr_output, "value=257", "too large server.worker_thread_count should be logged");
}

void test_unknown_key_rejected() {
    const test::TempDir dir("nebula-server-config-unknown-key");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file, "[server]\nport = 8080\nunknown_field = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "unknown key should fail");
    test::expect_contains(captured.stderr_output, "app config unknown key", "unknown key should be logged");
    test::expect_contains(captured.stderr_output, "key=\"server.unknown_field\"", "unknown key should be logged");
    test::expect_contains(captured.stderr_output, "line=3", "unknown key should be logged");
}

void test_multiple_unknown_keys_report_all_logs() {
    const test::TempDir dir("nebula-server-config-multiple-unknown-keys");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "port = 8080\n"
                     "unknown_server = 1\n"
                     "\n"
                     "[logger]\n"
                     "unknown_logger = true\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "multiple unknown keys should fail");
    test::expect_contains(captured.stderr_output, "key=\"server.unknown_server\"",
                          "earliest unknown key should be logged");
    test::expect_contains(captured.stderr_output, "line=3", "earliest unknown key should be logged");
    test::expect_contains(captured.stderr_output, "key=\"logger.unknown_logger\"",
                          "later unknown key should also be logged");
    test::expect_contains(captured.stderr_output, "line=6", "later unknown key should also be logged");
}

void test_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file, "[server]\nport = \"8080\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch", "type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"server.port\"", "type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"integer\"", "type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=2", "type mismatch should be logged");
}

void test_out_of_range_rejected() {
    const test::TempDir dir("nebula-server-config-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file, "[server]\nport = 70000\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "out of range should fail");
    test::expect_contains(captured.stderr_output, "app config value out of range", "out of range should be logged");
    test::expect_contains(captured.stderr_output, "key=\"server.port\"", "out of range should be logged");
    test::expect_contains(captured.stderr_output, "value=70000", "out of range should be logged");
    test::expect_contains(captured.stderr_output, "line=2", "out of range should be logged");
}

void test_port_zero_rejected() {
    const test::TempDir dir("nebula-server-config-port-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file, "[server]\nport = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "port zero should fail");
    test::expect_contains(captured.stderr_output, "server config value out of range", "port zero should be logged");
    test::expect_contains(captured.stderr_output, "key=\"port\"", "port zero should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "port zero should be logged");
}

void test_root_default_path_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-root-default-path-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n"
                     "root_default_path = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "root_default_path type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch",
                          "root_default_path type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"routes.root_default_path\"",
                          "root_default_path type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"string\"",
                          "root_default_path type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=3", "root_default_path type mismatch should be logged");
}

void test_enable_healthz_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-enable-healthz-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_healthz = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "enable_healthz type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch",
                          "enable_healthz type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"routes.enable_healthz\"",
                          "enable_healthz type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"boolean\"",
                          "enable_healthz type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=2", "enable_healthz type mismatch should be logged");
}

void test_enable_echo_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-enable-echo-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_echo = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "enable_echo type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch",
                          "enable_echo type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"routes.enable_echo\"",
                          "enable_echo type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"boolean\"", "enable_echo type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=2", "enable_echo type mismatch should be logged");
}

void test_enable_root_default_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-enable-root-default-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "enable_root_default type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch",
                          "enable_root_default type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"routes.enable_root_default\"",
                          "enable_root_default type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"boolean\"",
                          "enable_root_default type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=2", "enable_root_default type mismatch should be logged");
}

void test_root_default_path_empty_rejected() {
    const test::TempDir dir("nebula-server-config-root-default-path-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n"
                     "root_default_path = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty root_default_path should fail");
    test::expect_contains(captured.stderr_output, "route config value invalid",
                          "empty root_default_path should be logged");
    test::expect_contains(captured.stderr_output, "key=\"root_default_path\"",
                          "empty root_default_path should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_path\"", "empty root_default_path should be logged");
}

void test_root_default_path_without_leading_slash_rejected() {
    const test::TempDir dir("nebula-server-config-root-default-path-leading-slash");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n"
                     "root_default_path = \"healthz\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "root_default_path without leading slash should fail");
    test::expect_contains(captured.stderr_output, "route config value invalid",
                          "root_default_path without leading slash should be logged");
    test::expect_contains(captured.stderr_output, "key=\"root_default_path\"",
                          "root_default_path without leading slash should be logged");
    test::expect_contains(captured.stderr_output, "error=\"must_start_with_slash\"",
                          "root_default_path without leading slash should be logged");
}

void test_root_default_path_self_mapping_rejected() {
    const test::TempDir dir("nebula-server-config-root-default-path-self");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n"
                     "root_default_path = \"/\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "root_default_path self mapping should fail");
    test::expect_contains(captured.stderr_output, "route config value invalid",
                          "root_default_path self mapping should be logged");
    test::expect_contains(captured.stderr_output, "key=\"root_default_path\"",
                          "root_default_path self mapping should be logged");
    test::expect_contains(captured.stderr_output, "error=\"self_mapping_not_allowed\"",
                          "root_default_path self mapping should be logged");
}

void test_root_default_path_template_rejected() {
    const test::TempDir dir("nebula-server-config-root-default-path-template");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n"
                     "root_default_path = \"/users/{id}\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "root_default_path template should fail");
    test::expect_contains(captured.stderr_output, "route config value invalid",
                          "root_default_path template should be logged");
    test::expect_contains(captured.stderr_output, "key=\"root_default_path\"",
                          "root_default_path template should be logged");
    test::expect_contains(captured.stderr_output, "error=\"path_template_not_allowed\"",
                          "root_default_path template should be logged");
}

void test_enable_root_default_true_without_path_keeps_default_target() {
    const test::TempDir dir("nebula-server-config-root-default-path-default-explicit-enable");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = true\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok,
                      "enable_root_default=true should keep the default target when path is omitted");
    test::expect_true(captured.result.config.routes.enable_root_default, "enable_root_default should stay enabled");
    test::expect_equal(captured.result.config.routes.root_default_path, std::string("/healthz"),
                       "root_default_path should keep default when omitted");
}

void test_root_default_path_missing_keeps_default_when_enable_root_default_uses_default_value() {
    const test::TempDir dir("nebula-server-config-root-default-path-default-implicit");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[server]\n"
                     "port = 8081\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok,
                      "missing root_default_path should keep default when enable_root_default is default");
    test::expect_equal(captured.result.config.server.port, static_cast<std::uint16_t>(8081), "port should map");
    test::expect_true(captured.result.config.routes.enable_root_default,
                      "enable_root_default should keep default true");
    test::expect_equal(captured.result.config.routes.root_default_path, std::string("/healthz"),
                       "root_default_path should keep default when not configured");
}

void test_root_default_path_without_explicit_enable_uses_existing_default_state() {
    const test::TempDir dir("nebula-server-config-root-default-path-implicit-enable");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "root_default_path = \"/custom-root\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok, "root_default_path should reuse the default enabled state");
    test::expect_true(captured.result.config.routes.enable_root_default,
                      "enable_root_default should keep default true");
    test::expect_equal(captured.result.config.routes.root_default_path, std::string("/custom-root"),
                       "root_default_path should map without requiring an extra enable flag");
}

void test_root_default_path_is_allowed_when_enable_root_default_false() {
    const test::TempDir dir("nebula-server-config-root-default-path-disabled-custom");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "root_default_path = \"/custom-root\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok, "root_default_path should be allowed when the route is disabled");
    test::expect_true(!captured.result.config.routes.enable_root_default, "enable_root_default should map");
    test::expect_equal(captured.result.config.routes.root_default_path, std::string("/custom-root"),
                       "root_default_path should still map when the route is disabled");
}

void test_root_default_path_missing_allowed_when_enable_root_default_false() {
    const test::TempDir dir("nebula-server-config-root-default-path-disabled");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok,
                      "missing root_default_path should be allowed when enable_root_default is false");
    test::expect_true(!captured.result.config.routes.enable_root_default, "enable_root_default should map");
    test::expect_equal(captured.result.config.routes.root_default_path, std::string("/healthz"),
                       "root_default_path should keep default when enable_root_default is false");
}

void test_database_host_empty_rejected() {
    const test::TempDir dir("nebula-server-config-database-host-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "host = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty database.host should fail");
    test::expect_contains(captured.stderr_output, "database config value invalid",
                          "empty database.host should be logged");
    test::expect_contains(captured.stderr_output, "key=\"host\"", "empty database.host should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"", "empty database.host should be logged");
}

void test_database_port_zero_rejected() {
    const test::TempDir dir("nebula-server-config-database-port-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "port = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero database.port should fail");
    test::expect_contains(captured.stderr_output, "database config value out of range",
                          "zero database.port should be logged");
    test::expect_contains(captured.stderr_output, "key=\"port\"", "zero database.port should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero database.port should be logged");
}

void test_database_name_empty_rejected() {
    const test::TempDir dir("nebula-server-config-database-name-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "name = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty database.name should fail");
    test::expect_contains(captured.stderr_output, "database config value invalid",
                          "empty database.name should be logged");
    test::expect_contains(captured.stderr_output, "key=\"name\"", "empty database.name should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"", "empty database.name should be logged");
}

void test_database_user_empty_rejected() {
    const test::TempDir dir("nebula-server-config-database-user-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "user = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty database.user should fail");
    test::expect_contains(captured.stderr_output, "database config value invalid",
                          "empty database.user should be logged");
    test::expect_contains(captured.stderr_output, "key=\"user\"", "empty database.user should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"", "empty database.user should be logged");
}

void test_database_max_connections_zero_rejected() {
    const test::TempDir dir("nebula-server-config-database-pool-size-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "max_connections = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero database.max_connections should fail");
    test::expect_contains(captured.stderr_output, "database config value out of range",
                          "zero database.max_connections should be logged");
    test::expect_contains(captured.stderr_output, "key=\"max_connections\"",
                          "zero database.max_connections should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero database.max_connections should be logged");
}

void test_database_connect_timeout_zero_rejected() {
    const test::TempDir dir("nebula-server-config-database-connect-timeout-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "connect_timeout_s = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero database.connect_timeout_s should fail");
    test::expect_contains(captured.stderr_output, "database config value out of range",
                          "zero database.connect_timeout_s should be logged");
    test::expect_contains(captured.stderr_output, "key=\"connect_timeout_s\"",
                          "zero database.connect_timeout_s should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero database.connect_timeout_s should be logged");
}

void test_database_acquire_timeout_zero_rejected() {
    const test::TempDir dir("nebula-server-config-database-acquire-timeout-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "acquire_timeout_ms = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero database.acquire_timeout_ms should fail");
    test::expect_contains(captured.stderr_output, "database config value out of range",
                          "zero database.acquire_timeout_ms should be logged");
    test::expect_contains(captured.stderr_output, "key=\"acquire_timeout_ms\"",
                          "zero database.acquire_timeout_ms should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero database.acquire_timeout_ms should be logged");
}

void test_database_legacy_pool_size_key_rejected() {
    const test::TempDir dir("nebula-server-config-database-legacy-pool-size");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[database]\n"
                     "pool_size = 8\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "legacy database.pool_size should be rejected");
    test::expect_contains(captured.stderr_output, "app config unknown key",
                          "legacy database.pool_size should be logged as unknown");
    test::expect_contains(captured.stderr_output, "key=\"database.pool_size\"",
                          "legacy database.pool_size should be logged as unknown");
    test::expect_contains(captured.stderr_output, "line=2", "legacy database.pool_size should be logged as unknown");
}

void test_database_legacy_password_key_rejected() {
    const test::TempDir dir("nebula-server-config-database-legacy-password");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[database]\n"
                     "password = \"nebula\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "legacy database.password should be rejected");
    test::expect_contains(captured.stderr_output, "app config unknown key",
                          "legacy database.password should be logged as unknown");
    test::expect_contains(captured.stderr_output, "key=\"database.password\"",
                          "legacy database.password should be logged as unknown");
    test::expect_contains(captured.stderr_output, "line=2", "legacy database.password should be logged as unknown");
}

void test_database_password_env_present_loads_successfully() {
    const test::TempDir dir("nebula-server-config-database-password-env");
    const std::filesystem::path file = dir.path() / "server.toml";
    ::setenv("NEBULA_TEST_PASSWORD_FROM_ENV", "password_from_env", 1);
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "password_env = \"NEBULA_TEST_PASSWORD_FROM_ENV\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    ::unsetenv("NEBULA_TEST_PASSWORD_FROM_ENV");
    test::expect_true(captured.result.ok, "database.password_env should resolve during config load");
    test::expect_equal(captured.result.config.database.password, std::string("password_from_env"),
                       "database password should contain resolved env value");
}

void test_database_password_env_omitted_uses_default_env() {
    const test::TempDir dir("nebula-server-config-database-password-default-env");
    const std::filesystem::path file = dir.path() / "server.toml";
    const test::ScopedEnvVar database_password("NEBULA_DATABASE_PASSWORD", "default_password_from_env");
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(captured.result.ok, "omitted database.password_env should keep config load successful");
    test::expect_equal(captured.result.config.database.password, std::string("default_password_from_env"),
                       "omitted database.password_env should use default env");
    test::expect_not_contains(captured.stderr_output, "database password env unresolved",
                              "omitted database.password_env should not log unresolved warning");
}

void test_database_password_env_missing_rejected() {
    const test::TempDir dir("nebula-server-config-database-password-env-missing");
    const std::filesystem::path file = dir.path() / "server.toml";
    ::unsetenv("NEBULA_TEST_PASSWORD_FROM_ENV_MISSING");
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "password_env = \"NEBULA_TEST_PASSWORD_FROM_ENV_MISSING\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "missing database.password env target should fail config load");
    test::expect_contains(captured.stderr_output, "database password resolve failed",
                          "missing database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "key=\"database.password_env\"",
                          "missing database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "env_name=\"NEBULA_TEST_PASSWORD_FROM_ENV_MISSING\"",
                          "missing database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "error=\"env_not_set\"",
                          "missing database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "key=\"password\"",
                          "missing database.password env should invalidate password");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"",
                          "missing database.password env should keep stable invalid config error");
}

void test_database_password_env_empty_rejected() {
    const test::TempDir dir("nebula-server-config-database-password-env-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    const test::ScopedEnvVar database_password("NEBULA_TEST_PASSWORD_FROM_ENV_EMPTY", "");
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[database]\n"
                     "password_env = \"NEBULA_TEST_PASSWORD_FROM_ENV_EMPTY\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty database.password env target should fail config load");
    test::expect_contains(captured.stderr_output, "database password resolve failed",
                          "empty database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "key=\"database.password_env\"",
                          "empty database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "env_name=\"NEBULA_TEST_PASSWORD_FROM_ENV_EMPTY\"",
                          "empty database.password_env target should be logged");
    test::expect_contains(captured.stderr_output, "error=\"env_empty\"",
                          "empty database.password_env target should use dedicated error");
    test::expect_contains(captured.stderr_output, "key=\"password\"",
                          "empty database.password env should invalidate password");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"",
                          "empty database.password env should keep stable invalid config error");
}

void test_auth_jwt_secret_path_empty_rejected() {
    const test::TempDir dir("nebula-server-config-auth-jwt-path-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "jwt_secret_path = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty auth.jwt_secret_path should fail");
    test::expect_contains(captured.stderr_output, "auth config value invalid",
                          "empty auth.jwt_secret_path should be logged");
    test::expect_contains(captured.stderr_output, "key=\"jwt_secret_path\"",
                          "empty auth.jwt_secret_path should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"",
                          "empty auth.jwt_secret_path should be logged");
}

void test_auth_access_token_ttl_below_minimum_rejected() {
    const test::TempDir dir("nebula-server-config-auth-token-ttl-below-min");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "access_token_ttl_s = 59\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "below minimum auth.access_token_ttl_s should fail");
    test::expect_contains(captured.stderr_output, "auth config value out of range",
                          "below minimum auth.access_token_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "key=\"access_token_ttl_s\"",
                          "below minimum auth.access_token_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "value=59", "below minimum auth.access_token_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "min_value=60",
                          "below minimum auth.access_token_ttl_s should log min boundary");
}

void test_auth_access_token_ttl_out_of_range_rejected() {
    const test::TempDir dir("nebula-server-config-auth-token-ttl-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "access_token_ttl_s = 2147483648\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "too large auth.access_token_ttl_s should fail");
    test::expect_contains(captured.stderr_output, "auth config value out of range",
                          "too large auth.access_token_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "key=\"access_token_ttl_s\"",
                          "too large auth.access_token_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "value=2147483648",
                          "too large auth.access_token_ttl_s should be logged");
}

void test_auth_password_hash_iterations_type_mismatch_rejected() {
    const test::TempDir dir("nebula-server-config-auth-iterations-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "password_hash_iterations = \"120000\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "auth.password_hash_iterations type mismatch should fail");
    test::expect_contains(captured.stderr_output, "app config type mismatch",
                          "auth.password_hash_iterations type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "key=\"auth.password_hash_iterations\"",
                          "auth.password_hash_iterations type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "expected=\"integer\"",
                          "auth.password_hash_iterations type mismatch should be logged");
    test::expect_contains(captured.stderr_output, "line=5",
                          "auth.password_hash_iterations type mismatch should be logged");
}

void test_auth_password_hash_iterations_below_minimum_rejected() {
    const test::TempDir dir("nebula-server-config-auth-iterations-below-min");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "password_hash_iterations = 9999\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "below minimum auth.password_hash_iterations should fail");
    test::expect_contains(captured.stderr_output, "auth config value out of range",
                          "below minimum auth.password_hash_iterations should be logged");
    test::expect_contains(captured.stderr_output, "key=\"password_hash_iterations\"",
                          "below minimum auth.password_hash_iterations should be logged");
    test::expect_contains(captured.stderr_output, "value=9999",
                          "below minimum auth.password_hash_iterations should be logged");
}

void test_auth_password_hash_iterations_out_of_range_rejected() {
    const test::TempDir dir("nebula-server-config-auth-iterations-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[auth]\n"
                     "password_hash_iterations = 2147483648\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "too large auth.password_hash_iterations should fail");
    test::expect_contains(captured.stderr_output, "auth config value out of range",
                          "too large auth.password_hash_iterations should be logged");
    test::expect_contains(captured.stderr_output, "key=\"password_hash_iterations\"",
                          "too large auth.password_hash_iterations should be logged");
    test::expect_contains(captured.stderr_output, "value=2147483648",
                          "too large auth.password_hash_iterations should be logged");
}

void test_storage_root_dir_empty_rejected() {
    const test::TempDir dir("nebula-server-config-storage-root-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[storage]\n"
                     "root_dir = \"\"\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "empty storage root_dir should fail");
    test::expect_contains(captured.stderr_output, "storage config value invalid",
                          "empty storage root_dir should be logged");
    test::expect_contains(captured.stderr_output, "key=\"root_dir\"", "empty storage root_dir should be logged");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"", "empty storage root_dir should be logged");
}

void test_storage_upload_session_ttl_s_must_be_positive() {
    const test::TempDir dir("nebula-server-config-storage-ttl-positive");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[storage]\n"
                     "upload_session_ttl_s = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero storage upload_session_ttl_s should fail");
    test::expect_contains(captured.stderr_output, "storage config value out of range",
                          "zero storage upload_session_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "key=\"upload_session_ttl_s\"",
                          "zero storage upload_session_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero storage upload_session_ttl_s should be logged");
}

void test_storage_download_ticket_ttl_s_must_be_positive() {
    const test::TempDir dir("nebula-server-config-storage-download-ticket-ttl-positive");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[storage]\n"
                     "download_ticket_ttl_s = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero storage download_ticket_ttl_s should fail");
    test::expect_contains(captured.stderr_output, "storage config value out of range",
                          "zero storage download_ticket_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "key=\"download_ticket_ttl_s\"",
                          "zero storage download_ticket_ttl_s should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero storage download_ticket_ttl_s should be logged");
}

void test_storage_max_file_size_must_be_positive() {
    const test::TempDir dir("nebula-server-config-storage-max-file-positive");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[storage]\n"
                     "max_file_mb = 0\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "zero storage max_file_mb should fail");
    test::expect_contains(captured.stderr_output, "storage config value out of range",
                          "zero storage max_file_mb should be logged");
    test::expect_contains(captured.stderr_output, "key=\"max_file_bytes\"",
                          "zero storage max_file_mb should be logged");
    test::expect_contains(captured.stderr_output, "value=0", "zero storage max_file_mb should be logged");
}

void test_storage_max_file_size_unit_must_be_unique() {
    const test::TempDir dir("nebula-server-config-storage-max-file-unit-unique");
    const std::filesystem::path file = dir.path() / "server.toml";
    test::write_file(file,
                     "[routes]\n"
                     "enable_root_default = false\n"
                     "\n"
                     "[storage]\n"
                     "max_file_bytes = 1024\n"
                     "max_file_kb = 1\n");

    const CapturedLoadResult captured = load_app_config_with_stderr(file);
    test::expect_true(!captured.result.ok, "multiple storage max file units should fail");
    test::expect_contains(captured.stderr_output, "app config value invalid",
                          "multiple storage max file units should be logged");
    test::expect_contains(captured.stderr_output, "key=\"storage.max_file_size\"",
                          "multiple storage max file units should be logged");
    test::expect_contains(captured.stderr_output, "line=5", "multiple storage max file units should be logged");
    test::expect_contains(captured.stderr_output, "error=\"multiple_units\"",
                          "multiple storage max file units should be logged");
}

void test_missing_file_fails() {
    const test::TempDir dir("nebula-server-config-required");
    const std::filesystem::path missing = dir.path() / "missing.toml";

    const CapturedLoadResult captured = load_app_config_with_stderr(missing);
    test::expect_true(!captured.result.ok, "missing file should fail");
    test::expect_equal(captured.result.source, app::AppConfigSource::File, "missing file should report file source");
    test::expect_contains(captured.stderr_output, "app config file not found", "missing file should be logged");
    test::expect_contains(captured.stderr_output, "error=\"config_file_not_found\"", "missing file should be logged");
}

int run_app_config_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"load full valid config", test_load_full_valid_config},
        {"max connections zero rejected", test_max_connections_zero_rejected},
        {"worker threads zero maps to default auto value", test_worker_thread_count_zero_maps_to_default_auto_value},
        {"io threads zero maps to default auto value", test_sub_reactor_count_zero_maps_to_default_auto_value},
        {"max connections above limit rejected", test_max_connections_above_limit_rejected},
        {"io threads above limit rejected", test_sub_reactor_count_above_limit_rejected},
        {"worker threads above limit rejected", test_worker_thread_count_above_limit_rejected},
        {"unknown key rejected", test_unknown_key_rejected},
        {"multiple unknown keys report all logs", test_multiple_unknown_keys_report_all_logs},
        {"type mismatch rejected", test_type_mismatch_rejected},
        {"out of range rejected", test_out_of_range_rejected},
        {"port zero rejected", test_port_zero_rejected},
        {"root default path type mismatch rejected", test_root_default_path_type_mismatch_rejected},
        {"enable healthz type mismatch rejected", test_enable_healthz_type_mismatch_rejected},
        {"enable echo type mismatch rejected", test_enable_echo_type_mismatch_rejected},
        {"enable root default type mismatch rejected", test_enable_root_default_type_mismatch_rejected},
        {"root default path empty rejected", test_root_default_path_empty_rejected},
        {"root default path without leading slash rejected", test_root_default_path_without_leading_slash_rejected},
        {"root default path self mapping rejected", test_root_default_path_self_mapping_rejected},
        {"root default path template rejected", test_root_default_path_template_rejected},
        {"enable root default true without path keeps default target",
         test_enable_root_default_true_without_path_keeps_default_target},
        {"root default path missing keeps default when enable root default uses default value",
         test_root_default_path_missing_keeps_default_when_enable_root_default_uses_default_value},
        {"root default path without explicit enable uses existing default state",
         test_root_default_path_without_explicit_enable_uses_existing_default_state},
        {"root default path is allowed when enable root default false",
         test_root_default_path_is_allowed_when_enable_root_default_false},
        {"root default path missing allowed when enable root default false",
         test_root_default_path_missing_allowed_when_enable_root_default_false},
        {"database host empty rejected", test_database_host_empty_rejected},
        {"database port zero rejected", test_database_port_zero_rejected},
        {"database name empty rejected", test_database_name_empty_rejected},
        {"database user empty rejected", test_database_user_empty_rejected},
        {"database max connections zero rejected", test_database_max_connections_zero_rejected},
        {"database connect timeout zero rejected", test_database_connect_timeout_zero_rejected},
        {"database acquire timeout zero rejected", test_database_acquire_timeout_zero_rejected},
        {"database legacy pool size key rejected", test_database_legacy_pool_size_key_rejected},
        {"database legacy password key rejected", test_database_legacy_password_key_rejected},
        {"database password env present loads successfully", test_database_password_env_present_loads_successfully},
        {"database password env omitted uses default env", test_database_password_env_omitted_uses_default_env},
        {"database password env missing rejected", test_database_password_env_missing_rejected},
        {"database password env empty rejected", test_database_password_env_empty_rejected},
        {"auth jwt secret path empty rejected", test_auth_jwt_secret_path_empty_rejected},
        {"auth access token ttl below minimum rejected", test_auth_access_token_ttl_below_minimum_rejected},
        {"auth access token ttl out of range rejected", test_auth_access_token_ttl_out_of_range_rejected},
        {"auth password hash iterations type mismatch rejected",
         test_auth_password_hash_iterations_type_mismatch_rejected},
        {"auth password hash iterations below minimum rejected",
         test_auth_password_hash_iterations_below_minimum_rejected},
        {"auth password hash iterations out of range rejected",
         test_auth_password_hash_iterations_out_of_range_rejected},
        {"storage root dir empty rejected", test_storage_root_dir_empty_rejected},
        {"storage upload session ttl must be positive", test_storage_upload_session_ttl_s_must_be_positive},
        {"storage download ticket ttl must be positive", test_storage_download_ticket_ttl_s_must_be_positive},
        {"storage max file size must be positive", test_storage_max_file_size_must_be_positive},
        {"storage max file size unit must be unique", test_storage_max_file_size_unit_must_be_unique},
        {"missing file fails", test_missing_file_fails},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_app_config_tests);
}
