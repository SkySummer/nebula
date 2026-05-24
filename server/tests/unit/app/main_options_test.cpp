#include "nebula/app/main_options.hpp"

#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

struct CapturedParseResult {
    std::optional<app::MainOptions> result;
    std::string stderr_output;
};

CapturedParseResult parse(const std::vector<std::string>& args) {
    test::ArgvBuilder argv(args);
    CapturedParseResult captured;
    captured.stderr_output = test::capture_stderr(
        [&]() { captured.result = nebula::app::parse_main_options(argv.span()); }, "nebula-main-options-test");
    return captured;
}

void test_default_options() {
    const CapturedParseResult captured = parse({"nebula"});
    if (!captured.result.has_value()) {
        test::fail("default args should parse");
    }
    const app::MainOptions& result = *captured.result;
    test::expect_true(!result.use_config_file, "default config should not be required");
    test::expect_true(result.config_path.empty(), "default config path should stay empty");
}

void test_config_argument() {
    const CapturedParseResult captured = parse({"nebula", "--config", "runtime/custom.toml"});
    if (!captured.result.has_value()) {
        test::fail("config arg should parse");
    }
    const app::MainOptions& result = *captured.result;
    test::expect_true(result.use_config_file, "explicit config should be required");
    test::expect_equal(result.config_path.generic_string(), std::string("runtime/custom.toml"),
                       "explicit config path should match");
}

void test_missing_config_path_rejected() {
    const CapturedParseResult captured = parse({"nebula", "--config"});
    test::expect_true(!captured.result.has_value(), "missing config path should fail");
    test::expect_contains(captured.stderr_output, "main option invalid", "missing config path should be logged");
    test::expect_contains(captured.stderr_output, "arg=\"--config\"", "missing config path should include option");
    test::expect_contains(captured.stderr_output, "error=\"missing_value\"",
                          "missing config path should include stable error");
}

void test_empty_config_path_rejected() {
    const CapturedParseResult captured = parse({"nebula", "--config", ""});
    test::expect_true(!captured.result.has_value(), "empty config path should fail");
    test::expect_contains(captured.stderr_output, "main option invalid", "empty config path should be logged");
    test::expect_contains(captured.stderr_output, "arg=\"--config\"", "empty config path should include option");
    test::expect_contains(captured.stderr_output, "error=\"empty_value\"",
                          "empty config path should include stable error");
}

void test_unknown_argument_rejected() {
    const CapturedParseResult captured = parse({"nebula", "--port", "8080"});
    test::expect_true(!captured.result.has_value(), "unknown arg should fail");
    test::expect_contains(captured.stderr_output, "main option invalid", "unknown arg should be logged");
    test::expect_contains(captured.stderr_output, "arg=\"--port\"", "unknown arg should include argument");
    test::expect_contains(captured.stderr_output, "error=\"unknown_argument\"",
                          "unknown arg should include stable error");
}

void test_duplicate_config_argument_rejected() {
    const CapturedParseResult captured = parse({"nebula", "--config", "a.toml", "--config", "b.toml"});
    test::expect_true(!captured.result.has_value(), "duplicate config arg should fail");
    test::expect_contains(captured.stderr_output, "main option invalid", "duplicate config arg should be logged");
    test::expect_contains(captured.stderr_output, "arg=\"--config\"", "duplicate config arg should include option");
    test::expect_contains(captured.stderr_output, "error=\"duplicate_argument\"",
                          "duplicate config arg should include stable error");
}

int run_main_options_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"default options", test_default_options},
        {"config argument", test_config_argument},
        {"missing config path rejected", test_missing_config_path_rejected},
        {"empty config path rejected", test_empty_config_path_rejected},
        {"unknown argument rejected", test_unknown_argument_rejected},
        {"duplicate config argument rejected", test_duplicate_config_argument_rejected},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_main_options_tests);
}
