#include "nebula/server/server_config.hpp"

#include <fstream>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::LogLevel;
using nebula::server::ServerConfig;
using nebula::server::ServerConfigSource;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path);
    expect_true(stream.is_open(), "config file should open for write");
    stream << content;
    stream.flush();
    expect_true(stream.good(), "config file should flush successfully");
}

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
               "level = \"wArN\"\n"
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
               "root_default_path = \"/healthz\"\n");

    const nebula::server::ServerConfigLoadResult loaded(file);
    expect_true(loaded.ok, "valid config should load");
    expect_equal(loaded.source, ServerConfigSource::File, "valid config should report file source");

    const ServerConfig& config = loaded.config;
    expect_equal(config.port, static_cast<std::uint16_t>(9090), "port should map");
    expect_equal(config.backlog, 2048, "backlog should map");
    expect_equal(config.max_connections, static_cast<std::size_t>(5000), "max_connections should map");
    expect_equal(config.sub_reactor_count, static_cast<std::size_t>(3), "sub_reactor_count should map");
    expect_equal(config.worker_thread_count, static_cast<std::size_t>(6), "worker_thread_count should map");
    expect_true(!config.manage_signals, "manage_signals should map");
    expect_equal(config.log_level, LogLevel::Warning, "log level should parse case-insensitive");
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

int run_server_config_loader_tests() {
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
        {"missing file fails", test_missing_file_fails},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_config_loader_tests);
}
