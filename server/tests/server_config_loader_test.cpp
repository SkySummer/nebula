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
               "max_body_bytes = 8192\n");

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
}

void test_worker_thread_count_zero_maps_to_default_auto_value() {
    const TempDir dir("nebula-server-config-worker-threads-zero");
    const std::filesystem::path file = dir.path() / "server.toml";
    write_file(file,
               "[server]\n"
               "worker_thread_count = 0\n");

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
               "sub_reactor_count = 0\n");

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
        {"missing file fails", test_missing_file_fails},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_config_loader_tests);
}
