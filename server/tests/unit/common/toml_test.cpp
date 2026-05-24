#include "nebula/common/codec/toml.hpp"

#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

const common::TomlValue* require_key(const common::TomlParseResult& result, const std::string& key) {
    const auto it = result.table.find(key);
    test::expect_true(it != result.table.end(), "key should exist");
    return &it->second;
}

void test_parse_sections_values_comments() {
    const std::string text =
        "# comment\n"
        "[server]\n"
        "port = 8080\n"
        "manage_signals = true\n"
        "ratio = 1.5\n"
        "\n"
        "[logger]\n"
        "level = \"Trace\"\n"
        "dir = \"runtime/logs\" # inline comment\n";

    const common::TomlParseResult parsed = nebula::common::parse_toml(text);
    test::expect_true(parsed.ok, "valid toml should parse");
    test::expect_equal(parsed.table.size(), std::size_t{5}, "table size should match");

    const common::TomlValue* port = require_key(parsed, "server.port");
    test::expect_equal(std::get<std::int64_t>(port->value), std::int64_t{8080}, "port should parse as integer");

    const common::TomlValue* manage_signals = require_key(parsed, "server.manage_signals");
    test::expect_true(std::get<bool>(manage_signals->value), "manage_signals should parse as bool");

    const common::TomlValue* ratio = require_key(parsed, "server.ratio");
    test::expect_equal(std::get<double>(ratio->value), 1.5, "ratio should parse as double");

    const common::TomlValue* level = require_key(parsed, "logger.level");
    test::expect_equal(std::get<std::string>(level->value), std::string("Trace"), "level should parse as string");

    const common::TomlValue* dir = require_key(parsed, "logger.dir");
    test::expect_equal(std::get<std::string>(dir->value), std::string("runtime/logs"), "dir should parse as string");
}

void test_parse_duplicate_key_rejected() {
    const std::string text = "[server]\nport = 1\nport = 2\n";
    const common::TomlParseResult parsed = nebula::common::parse_toml(text);

    test::expect_true(!parsed.ok, "duplicate key should fail");
    test::expect_equal(parsed.error_line, std::size_t{3}, "duplicate key should report line");
    test::expect_equal(parsed.error, std::string("duplicate_key"), "duplicate key should report fixed error");
}

void test_parse_invalid_key_rejected() {
    const std::string text = "[server]\nmax.body = 1\n";
    const common::TomlParseResult parsed = nebula::common::parse_toml(text);

    test::expect_true(!parsed.ok, "invalid key should fail");
    test::expect_equal(parsed.error_line, std::size_t{2}, "invalid key should report line");
    test::expect_equal(parsed.error, std::string("invalid_key"), "invalid key should report fixed error");
}

void test_parse_invalid_float_rejected() {
    const std::string text = "[server]\nratio = 1.5.2\n";
    const common::TomlParseResult parsed = nebula::common::parse_toml(text);

    test::expect_true(!parsed.ok, "invalid float should fail");
    test::expect_equal(parsed.error_line, std::size_t{2}, "invalid float should report line");
    test::expect_contains(parsed.error, "invalid_float", "invalid float should report float parse error");
}

void test_parse_unterminated_string_rejected() {
    const std::string text = "[logger]\nlevel = \"trace\n";
    const common::TomlParseResult parsed = nebula::common::parse_toml(text);

    test::expect_true(!parsed.ok, "unterminated string should fail");
    test::expect_equal(parsed.error_line, std::size_t{2}, "unterminated string should report line");
    test::expect_equal(parsed.error, std::string("unterminated_string"), "unterminated string should have fixed error");
}

int run_toml_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"parse sections values comments", test_parse_sections_values_comments},
        {"parse duplicate key rejected", test_parse_duplicate_key_rejected},
        {"parse invalid key rejected", test_parse_invalid_key_rejected},
        {"parse invalid float rejected", test_parse_invalid_float_rejected},
        {"parse unterminated string rejected", test_parse_unterminated_string_rejected},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_toml_tests);
}
