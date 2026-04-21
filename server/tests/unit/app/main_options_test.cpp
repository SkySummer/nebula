#include "nebula/app/main_options.hpp"

#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::app::MainOptionsParseResult;
using nebula::testsupport::ArgvBuilder;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

MainOptionsParseResult parse(const std::vector<std::string>& args) {
    ArgvBuilder argv(args);
    return nebula::app::parse_main_options(argv.span());
}

void test_default_options() {
    const auto parsed = parse({"nebula"});
    expect_true(parsed.ok, "default args should parse");
    expect_true(!parsed.options.config_required, "default config should not be required");
    expect_true(parsed.options.config_path.empty(), "default config path should stay empty");
}

void test_config_argument() {
    const auto parsed = parse({"nebula", "--config", "runtime/custom.toml"});
    expect_true(parsed.ok, "config arg should parse");
    expect_true(parsed.options.config_required, "explicit config should be required");
    expect_equal(parsed.options.config_path.string(), std::string("runtime/custom.toml"),
                 "explicit config path should match");
}

void test_missing_config_path_rejected() {
    const auto parsed = parse({"nebula", "--config"});
    expect_true(!parsed.ok, "missing config path should fail");
    expect_equal(parsed.error, std::string("missing_config_path"), "missing config path should return fixed error");
}

void test_empty_config_path_rejected() {
    const auto parsed = parse({"nebula", "--config", ""});
    expect_true(!parsed.ok, "empty config path should fail");
    expect_equal(parsed.error, std::string("empty_config_path"), "empty config path should return fixed error");
}

void test_unknown_argument_rejected() {
    const auto parsed = parse({"nebula", "--port", "8080"});
    expect_true(!parsed.ok, "unknown arg should fail");
    expect_contains(parsed.error, "unknown_argument", "unknown arg should return fixed prefix");
}

void test_duplicate_config_argument_rejected() {
    const auto parsed = parse({"nebula", "--config", "a.toml", "--config", "b.toml"});
    expect_true(!parsed.ok, "duplicate config arg should fail");
    expect_equal(parsed.error, std::string("duplicate_config_argument"),
                 "duplicate config arg should return fixed error");
}

int run_main_options_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"default options", test_default_options},
        {"config argument", test_config_argument},
        {"missing config path rejected", test_missing_config_path_rejected},
        {"empty config path rejected", test_empty_config_path_rejected},
        {"unknown argument rejected", test_unknown_argument_rejected},
        {"duplicate config argument rejected", test_duplicate_config_argument_rejected},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_main_options_tests);
}
