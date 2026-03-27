#include "nebula/server/server_app.hpp"

#include <span>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::TempDir;

int run_server_app_with_stderr(const std::vector<std::string>& args, std::string& stderr_output) {
    std::vector<std::string> storage = args;
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }

    int exit_code = -1;
    stderr_output = nebula::testsupport::capture_stderr(
        [&]() {
            nebula::server::ServerApp app(std::span<char*>(argv.data(), argv.size()));
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

int run_server_app_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"run rejected when main option invalid", test_run_rejected_when_main_option_invalid},
        {"run rejected when required config missing", test_run_rejected_when_required_config_missing},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_app_tests);
}
