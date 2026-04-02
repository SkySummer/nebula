#include "nebula/server/startup.hpp"

#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::server::ServerConfig;
using nebula::server::ServerConfigSource;
using nebula::server::StartupResult;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;

StartupResult startup_with_args(const std::vector<std::string>& args) {
    std::vector<std::string> storage = args;
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }
    return StartupResult(std::span<char*>(argv.data(), argv.size()));
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path);
    expect_true(stream.is_open(), "config file should open for write");
    stream << content;
    stream.flush();
    expect_true(stream.good(), "config file should flush successfully");
}

void test_default_startup_uses_default_config_without_file_loading() {
    const StartupResult startup = startup_with_args({"nebula"});
    expect_true(startup.ok, "default startup should succeed");
    expect_equal(startup.config_source, ServerConfigSource::Default, "default startup should use default source");
    expect_true(startup.config_path.empty(), "default startup should not carry config path");

    const ServerConfig defaults;
    expect_equal(startup.config.port, defaults.port, "default startup should keep default port");
    expect_equal(startup.config.backlog, defaults.backlog, "default startup should keep default backlog");
}

void test_startup_loads_file_when_config_argument_present() {
    const TempDir dir("nebula-startup-config");
    const std::filesystem::path config_file = dir.path() / "custom.toml";
    write_file(config_file,
               "[server]\n"
               "port = 9091\n"
               "backlog = 128\n"
               "\n"
               "[routes]\n"
               "root_default_path = \"/healthz\"\n");

    const StartupResult startup = startup_with_args({"nebula", "--config", config_file.string()});
    expect_true(startup.ok, "explicit config startup should succeed");
    expect_equal(startup.config_source, ServerConfigSource::File, "explicit config startup should use file source");
    expect_equal(startup.config_path, config_file, "explicit config startup should preserve config path");
    expect_equal(startup.config.port, static_cast<std::uint16_t>(9091), "explicit config startup should load port");
    expect_equal(startup.config.backlog, 128, "explicit config startup should load backlog");
}

void test_startup_fails_when_required_config_missing() {
    const TempDir dir("nebula-startup-missing-config");
    const std::filesystem::path missing = dir.path() / "missing.toml";

    const StartupResult startup = startup_with_args({"nebula", "--config", missing.string()});
    expect_true(!startup.ok, "missing required config should fail startup");
    expect_equal(startup.config_path, missing, "missing required config should keep requested path");
    expect_contains(startup.error, "load server config failed", "startup should include config load failure");
    expect_contains(startup.error, "decision=exit_process", "startup failure should include decision");
}

int run_startup_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"default startup uses default config without file loading",
         test_default_startup_uses_default_config_without_file_loading},
        {"startup loads file when config argument present", test_startup_loads_file_when_config_argument_present},
        {"startup fails when required config missing", test_startup_fails_when_required_config_missing},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_startup_tests);
}
