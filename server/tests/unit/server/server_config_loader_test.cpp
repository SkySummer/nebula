#include "nebula/server/server_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "nebula/common/logger.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::LogLevel;
using nebula::server::ServerConfig;
using nebula::server::ServerConfigSource;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_file;

void test_load_full_valid_config() {
    const TempDir dir("nebula-server-config-valid");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
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
               "[auth]\n"
               "jwt_secret_path = \"runtime/secrets/test_jwt.key\"\n"
               "access_token_ttl_s = 7200\n"
               "password_hash_iterations = 240000\n"
               "\n"
               "[database]\n"
               "host = \"db.local\"\n"
               "port = 15432\n"
               "name = \"nebula_test\"\n"
               "user = \"nebula_user\"\n"
               "password_env = \"NEBULA_TEST_DATABASE_PASSWORD_CFG\"\n"
               "max_connections = 12\n"
               "connect_timeout_ms = 2500\n"
               "acquire_timeout_ms = 3500\n");

    ::setenv("NEBULA_TEST_DATABASE_PASSWORD_CFG", "db_password", 1);
    const nebula::server::ServerConfigLoadResult loaded(file);
    ::unsetenv("NEBULA_TEST_DATABASE_PASSWORD_CFG");
    expect_true(loaded.ok, "valid config should load");
    expect_equal(loaded.source, ServerConfigSource::File, "valid config should report file source");

    const ServerConfig& config = loaded.config;
    expect_equal(config.port, static_cast<std::uint16_t>(9090), "port should map");
    expect_equal(config.backlog, 2048, "backlog should map");
    expect_equal(config.max_connections, static_cast<std::size_t>(5000), "max_connections should map");
    expect_equal(config.sub_reactor_count, static_cast<std::size_t>(3), "sub_reactor_count should map");
    expect_equal(config.worker_thread_count, static_cast<std::size_t>(6), "worker_thread_count should map");
    expect_true(!config.manage_signals, "manage_signals should map");
    expect_equal(config.log_level, LogLevel::Fatal, "log level should parse case-insensitive");
    expect_equal(config.log_dir.string(), std::string("runtime/custom-logs"), "log_dir should map");
    expect_true(!config.log_also_stderr, "also_stderr should map");
    expect_equal(config.read_timeout.count(), static_cast<std::int64_t>(12345), "read timeout should map");
    expect_equal(config.graceful_shutdown_timeout.count(), static_cast<std::int64_t>(2500),
                 "graceful shutdown timeout should map");
    expect_equal(config.max_header_bytes, static_cast<std::size_t>(4096), "max_header_bytes should map");
    expect_equal(config.max_request_target_bytes, static_cast<std::size_t>(2048),
                 "max_request_target_bytes should map");
    expect_equal(config.max_body_bytes, static_cast<std::size_t>(8192), "max_body_bytes should map");
    expect_true(!config.enable_healthz, "enable_healthz should map");
    expect_true(config.enable_echo, "enable_echo should map");
    expect_true(config.enable_root_default, "enable_root_default should map");
    expect_equal(config.root_default_path, std::string("/healthz"), "root_default_path should map");
    expect_equal(config.auth_jwt_secret_path.string(), std::string("runtime/secrets/test_jwt.key"),
                 "auth jwt secret path should map");
    expect_equal(config.auth_access_token_ttl_s, static_cast<std::int64_t>(7200), "auth ttl should map");
    expect_equal(config.auth_password_hash_iterations, static_cast<std::uint32_t>(240000),
                 "auth hash iterations should map");
    expect_equal(config.database_host, std::string("db.local"), "database host should map");
    expect_equal(config.database_port, static_cast<std::uint16_t>(15432), "database port should map");
    expect_equal(config.database_name, std::string("nebula_test"), "database name should map");
    expect_equal(config.database_user, std::string("nebula_user"), "database user should map");
    expect_equal(config.database_password_env, std::string("NEBULA_TEST_DATABASE_PASSWORD_CFG"),
                 "database password env should map");
    expect_equal(config.database_max_connections, static_cast<std::size_t>(12), "database max connections should map");
    expect_equal(config.database_connect_timeout_ms, static_cast<std::int64_t>(2500),
                 "database connect timeout should map");
    expect_equal(config.database_acquire_timeout_ms, static_cast<std::int64_t>(3500),
                 "database acquire timeout should map");
}

void test_worker_thread_count_zero_maps_to_default_auto_value() {
    const TempDir dir("nebula-server-config-worker-threads-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[server]\n"
               "worker_thread_count = 0\n"
               "\n"
               "[routes]\n"
               "enable_root_default = false\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(loaded.ok, "worker_thread_count zero should load");
    expect_equal(loaded.source, ServerConfigSource::File, "worker_thread_count zero should keep file source");
    expect_equal(loaded.config.worker_thread_count, nebula::server::default_worker_thread_count(),
                 "worker_thread_count zero should map to default auto value");
}

void test_sub_reactor_count_zero_maps_to_default_auto_value() {
    const TempDir dir("nebula-server-config-io-threads-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[server]\n"
               "sub_reactor_count = 0\n"
               "\n"
               "[routes]\n"
               "enable_root_default = false\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(loaded.ok, "sub_reactor_count zero should load");
    expect_equal(loaded.source, ServerConfigSource::File, "sub_reactor_count zero should keep file source");
    expect_equal(loaded.config.sub_reactor_count, nebula::server::default_sub_reactor_count(),
                 "sub_reactor_count zero should map to default auto value");
}

void test_unknown_key_rejected() {
    const TempDir dir("nebula-server-config-unknown-key");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file, "[server]\nport = 8080\nunknown_field = 1\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "unknown key should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(3), "unknown key should report source line");
    expect_contains(loaded.error, "unknown_key", "unknown key should return structured error");
}

void test_multiple_unknown_keys_report_stable_first_line() {
    const TempDir dir("nebula-server-config-multiple-unknown-keys");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[server]\n"
               "port = 8080\n"
               "unknown_server = 1\n"
               "\n"
               "[logger]\n"
               "unknown_logger = true\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "multiple unknown keys should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(3),
                 "multiple unknown keys should report earliest source line");
    expect_equal(loaded.error, std::string("unknown_key:server.unknown_server"),
                 "multiple unknown keys should report deterministic key");
}

void test_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file, "[server]\nport = \"8080\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "type mismatch should report line");
    expect_contains(loaded.error, "type_mismatch", "type mismatch should return structured error");
}

void test_out_of_range_rejected() {
    const TempDir dir("nebula-server-config-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file, "[server]\nport = 70000\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "out of range should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "out of range should report line");
    expect_contains(loaded.error, "value_out_of_range", "out of range should return structured error");
}

void test_port_zero_rejected() {
    const TempDir dir("nebula-server-config-port-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file, "[server]\nport = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "port zero should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "port zero should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:server.port"), "port zero should return fixed error");
}

void test_root_default_path_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-root-default-path-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "root_default_path = 1\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "root_default_path type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "type mismatch should report line");
    expect_equal(loaded.error, std::string("type_mismatch:routes.root_default_path"),
                 "root_default_path type mismatch should return fixed error");
}

void test_enable_healthz_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-enable-healthz-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_healthz = 1\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "enable_healthz type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "enable_healthz type mismatch should report line");
    expect_equal(loaded.error, std::string("type_mismatch:routes.enable_healthz"),
                 "enable_healthz type mismatch should return fixed error");
}

void test_enable_echo_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-enable-echo-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_echo = 1\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "enable_echo type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "enable_echo type mismatch should report line");
    expect_equal(loaded.error, std::string("type_mismatch:routes.enable_echo"),
                 "enable_echo type mismatch should return fixed error");
}

void test_enable_root_default_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-enable-root-default-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = 1\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "enable_root_default type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2),
                 "enable_root_default type mismatch should report line");
    expect_equal(loaded.error, std::string("type_mismatch:routes.enable_root_default"),
                 "enable_root_default type mismatch should return fixed error");
}

void test_root_default_path_empty_rejected() {
    const TempDir dir("nebula-server-config-root-default-path-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "root_default_path = \"\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "empty root_default_path should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "empty root_default_path should report line");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:empty_path"),
                 "empty root_default_path should return fixed error");
}

void test_root_default_path_without_leading_slash_rejected() {
    const TempDir dir("nebula-server-config-root-default-path-leading-slash");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "root_default_path = \"healthz\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "root_default_path without leading slash should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2),
                 "root_default_path without leading slash should report line");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:must_start_with_slash"),
                 "root_default_path without leading slash should return fixed error");
}

void test_root_default_path_self_mapping_rejected() {
    const TempDir dir("nebula-server-config-root-default-path-self");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "root_default_path = \"/\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "root_default_path self mapping should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "root_default_path self mapping should report line");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:self_mapping_not_allowed"),
                 "root_default_path self mapping should return fixed error");
}

void test_root_default_path_template_rejected() {
    const TempDir dir("nebula-server-config-root-default-path-template");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "root_default_path = \"/users/{id}\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "root_default_path template should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "root_default_path template should report line");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:path_template_not_allowed"),
                 "root_default_path template should return fixed error");
}

void test_root_default_path_required_when_enable_root_default_true() {
    const TempDir dir("nebula-server-config-root-default-path-required");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = true\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "missing root_default_path should fail when enable_root_default is true");
    expect_equal(loaded.error_line, static_cast<std::size_t>(0),
                 "missing root_default_path should report no concrete source line");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:required_when_enable_root_default"),
                 "missing root_default_path should return fixed error");
}

void test_root_default_path_required_when_enable_root_default_default_true() {
    const TempDir dir("nebula-server-config-root-default-path-required-default");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[server]\n"
               "port = 8081\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "missing root_default_path should fail when enable_root_default stays default true");
    expect_equal(loaded.error, std::string("invalid_value:routes.root_default_path:required_when_enable_root_default"),
                 "missing root_default_path with default enable_root_default should return fixed error");
}

void test_root_default_path_missing_allowed_when_enable_root_default_false() {
    const TempDir dir("nebula-server-config-root-default-path-disabled");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(loaded.ok, "missing root_default_path should be allowed when enable_root_default is false");
    expect_true(!loaded.config.enable_root_default, "enable_root_default should map");
    expect_equal(loaded.config.root_default_path, std::string("/healthz"),
                 "root_default_path should keep default when enable_root_default is false");
}

void test_missing_file_fails() {
    const TempDir dir("nebula-server-config-required");
    const std::filesystem::path missing = dir.path() / "missing.toml";

    const nebula::server::ServerConfigLoadResult loaded(missing);
    expect_true(!loaded.ok, "missing file should fail");
    expect_equal(loaded.source, ServerConfigSource::File, "missing file should report file source");
    expect_equal(loaded.error, std::string("config_file_not_found"), "missing file should return fixed error");
}

void test_auth_jwt_secret_path_empty_rejected() {
    const TempDir dir("nebula-server-config-auth-jwt-path-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "jwt_secret_path = \"\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "empty auth.jwt_secret_path should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "empty auth.jwt_secret_path should report line");
    expect_equal(loaded.error, std::string("invalid_value:auth.jwt_secret_path:empty_value"),
                 "empty auth.jwt_secret_path should return fixed error");
}

void test_auth_access_token_ttl_zero_rejected() {
    const TempDir dir("nebula-server-config-auth-token-ttl-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "access_token_ttl_s = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "zero auth.access_token_ttl_s should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "zero auth.access_token_ttl_s should report line");
    expect_equal(loaded.error, std::string("invalid_value:auth.access_token_ttl_s:must_be_positive"),
                 "zero auth.access_token_ttl_s should return fixed error");
}

void test_auth_access_token_ttl_out_of_range_rejected() {
    const TempDir dir("nebula-server-config-auth-token-ttl-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "access_token_ttl_s = 2147483648\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "too large auth.access_token_ttl_s should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5),
                 "too large auth.access_token_ttl_s should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:auth.access_token_ttl_s"),
                 "too large auth.access_token_ttl_s should return fixed error");
}

void test_auth_password_hash_iterations_type_mismatch_rejected() {
    const TempDir dir("nebula-server-config-auth-iterations-type");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "password_hash_iterations = \"120000\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "auth.password_hash_iterations type mismatch should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5),
                 "auth.password_hash_iterations type mismatch should report line");
    expect_equal(loaded.error, std::string("type_mismatch:auth.password_hash_iterations"),
                 "auth.password_hash_iterations type mismatch should return fixed error");
}

void test_auth_password_hash_iterations_below_minimum_rejected() {
    const TempDir dir("nebula-server-config-auth-iterations-below-min");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "password_hash_iterations = 9999\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "below minimum auth.password_hash_iterations should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5),
                 "below minimum auth.password_hash_iterations should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:auth.password_hash_iterations"),
                 "below minimum auth.password_hash_iterations should return fixed error");
}

void test_auth_password_hash_iterations_out_of_range_rejected() {
    const TempDir dir("nebula-server-config-auth-iterations-range");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[auth]\n"
               "password_hash_iterations = 2147483648\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "too large auth.password_hash_iterations should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5),
                 "too large auth.password_hash_iterations should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:auth.password_hash_iterations"),
                 "too large auth.password_hash_iterations should return fixed error");
}

void test_database_host_empty_rejected() {
    const TempDir dir("nebula-server-config-database-host-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "host = \"\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "empty database.host should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "empty database.host should report line");
    expect_equal(loaded.error, std::string("invalid_value:database.host:empty_value"),
                 "empty database.host should return fixed error");
}

void test_database_port_zero_rejected() {
    const TempDir dir("nebula-server-config-database-port-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "port = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "zero database.port should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "zero database.port should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:database.port"),
                 "zero database.port should return fixed error");
}

void test_database_name_empty_rejected() {
    const TempDir dir("nebula-server-config-database-name-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "name = \"\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "empty database.name should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "empty database.name should report line");
    expect_equal(loaded.error, std::string("invalid_value:database.name:empty_value"),
                 "empty database.name should return fixed error");
}

void test_database_user_empty_rejected() {
    const TempDir dir("nebula-server-config-database-user-empty");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "user = \"\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "empty database.user should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "empty database.user should report line");
    expect_equal(loaded.error, std::string("invalid_value:database.user:empty_value"),
                 "empty database.user should return fixed error");
}

void test_database_max_connections_zero_rejected() {
    const TempDir dir("nebula-server-config-database-pool-size-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "max_connections = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "zero database.max_connections should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "zero database.max_connections should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:database.max_connections"),
                 "zero database.max_connections should return fixed error");
}

void test_database_connect_timeout_zero_rejected() {
    const TempDir dir("nebula-server-config-database-connect-timeout-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "connect_timeout_ms = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "zero database.connect_timeout_ms should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "zero database.connect_timeout_ms should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:database.connect_timeout_ms"),
                 "zero database.connect_timeout_ms should return fixed error");
}

void test_database_acquire_timeout_zero_rejected() {
    const TempDir dir("nebula-server-config-database-acquire-timeout-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "acquire_timeout_ms = 0\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "zero database.acquire_timeout_ms should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5), "zero database.acquire_timeout_ms should report line");
    expect_equal(loaded.error, std::string("value_out_of_range:database.acquire_timeout_ms"),
                 "zero database.acquire_timeout_ms should return fixed error");
}

void test_database_legacy_pool_size_key_rejected() {
    const TempDir dir("nebula-server-config-database-legacy-pool-size");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[database]\n"
               "pool_size = 8\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "legacy database.pool_size should be rejected");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "legacy database.pool_size should report line");
    expect_equal(loaded.error, std::string("unknown_key:database.pool_size"),
                 "legacy database.pool_size should report unknown key");
}

void test_database_legacy_password_key_rejected() {
    const TempDir dir("nebula-server-config-database-legacy-password");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[database]\n"
               "password = \"nebula\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "legacy database.password should be rejected");
    expect_equal(loaded.error_line, static_cast<std::size_t>(2), "legacy database.password should report line");
    expect_equal(loaded.error, std::string("unknown_key:database.password"),
                 "legacy database.password should report unknown key");
}

void test_database_password_env_missing_rejected() {
    const TempDir dir("nebula-server-config-database-password-missing");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "password_env = \"NEBULA_TEST_MISSING_PASSWORD\"\n");

    ::unsetenv("NEBULA_TEST_MISSING_PASSWORD");
    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(!loaded.ok, "missing database.password_env should fail");
    expect_equal(loaded.error_line, static_cast<std::size_t>(5),
                 "missing database.password_env should report source line");
    expect_equal(loaded.error, std::string("invalid_value:database.password_env:env_not_set"),
                 "missing database.password_env should return fixed error");
}

void test_database_password_env_present_loads_successfully() {
    const TempDir dir("nebula-server-config-database-password-env");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[routes]\n"
               "enable_root_default = false\n"
               "\n"
               "[database]\n"
               "password_env = \"NEBULA_TEST_PASSWORD_FROM_ENV\"\n");

    ::setenv("NEBULA_TEST_PASSWORD_FROM_ENV", "from_env", 1);
    const nebula::server::ServerConfigLoadResult loaded(file);
    ::unsetenv("NEBULA_TEST_PASSWORD_FROM_ENV");
    expect_true(loaded.ok, "database.password_env should satisfy password requirement");
}

int run_server_config_loader_tests() {
    ::setenv("NEBULA_DATABASE_PASSWORD", "unit_test_default_password", 1);
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"load full valid config", test_load_full_valid_config},
        {"io threads zero maps to default auto value", test_sub_reactor_count_zero_maps_to_default_auto_value},
        {"worker threads zero maps to default auto value", test_worker_thread_count_zero_maps_to_default_auto_value},
        {"unknown key rejected", test_unknown_key_rejected},
        {"multiple unknown keys report stable first line", test_multiple_unknown_keys_report_stable_first_line},
        {"type mismatch rejected", test_type_mismatch_rejected},
        {"out of range rejected", test_out_of_range_rejected},
        {"port zero rejected", test_port_zero_rejected},
        {"enable healthz type mismatch rejected", test_enable_healthz_type_mismatch_rejected},
        {"enable echo type mismatch rejected", test_enable_echo_type_mismatch_rejected},
        {"enable root default type mismatch rejected", test_enable_root_default_type_mismatch_rejected},
        {"root default path type mismatch rejected", test_root_default_path_type_mismatch_rejected},
        {"root default path empty rejected", test_root_default_path_empty_rejected},
        {"root default path without leading slash rejected", test_root_default_path_without_leading_slash_rejected},
        {"root default path self mapping rejected", test_root_default_path_self_mapping_rejected},
        {"root default path template rejected", test_root_default_path_template_rejected},
        {"root default path required when enable root default true",
         test_root_default_path_required_when_enable_root_default_true},
        {"root default path required when enable root default default true",
         test_root_default_path_required_when_enable_root_default_default_true},
        {"root default path missing allowed when enable root default false",
         test_root_default_path_missing_allowed_when_enable_root_default_false},
        {"auth jwt secret path empty rejected", test_auth_jwt_secret_path_empty_rejected},
        {"auth access token ttl zero rejected", test_auth_access_token_ttl_zero_rejected},
        {"auth access token ttl out of range rejected", test_auth_access_token_ttl_out_of_range_rejected},
        {"auth password hash iterations type mismatch rejected",
         test_auth_password_hash_iterations_type_mismatch_rejected},
        {"auth password hash iterations below minimum rejected",
         test_auth_password_hash_iterations_below_minimum_rejected},
        {"auth password hash iterations out of range rejected",
         test_auth_password_hash_iterations_out_of_range_rejected},
        {"database host empty rejected", test_database_host_empty_rejected},
        {"database port zero rejected", test_database_port_zero_rejected},
        {"database name empty rejected", test_database_name_empty_rejected},
        {"database user empty rejected", test_database_user_empty_rejected},
        {"database max connections zero rejected", test_database_max_connections_zero_rejected},
        {"database connect timeout zero rejected", test_database_connect_timeout_zero_rejected},
        {"database acquire timeout zero rejected", test_database_acquire_timeout_zero_rejected},
        {"database legacy pool size key rejected", test_database_legacy_pool_size_key_rejected},
        {"database legacy password key rejected", test_database_legacy_password_key_rejected},
        {"database password env missing rejected", test_database_password_env_missing_rejected},
        {"database password env present loads successfully", test_database_password_env_present_loads_successfully},
        {"missing file fails", test_missing_file_fails},
    };

    const int test_exit = nebula::testsupport::run_tests(tests);
    ::unsetenv("NEBULA_DATABASE_PASSWORD");
    return test_exit;
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_config_loader_tests);
}
