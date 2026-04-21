#include "nebula/common/toml_parser.hpp"

#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::TomlParseResult;
using nebula::common::TomlValue;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

const TomlValue* require_key(const TomlParseResult& result, const std::string& key) {
    const auto it = result.table.find(key);
    expect_true(it != result.table.end(), "key should exist");
    return &it->second;
}

void test_parse_sections_values_comments() {
    const std::string text =
        "# comment\n"
        "[server]\n"
        "port = 8080\n"
        "manage_signals = true\n"
        "\n"
        "[logger]\n"
        "level = \"Trace\"\n"
        "dir = \"runtime/logs\" # inline comment\n";

    const TomlParseResult parsed = nebula::common::parse_toml(text);
    expect_true(parsed.ok, "valid toml should parse");
    expect_equal(parsed.table.size(), static_cast<std::size_t>(4), "table size should match");

    const TomlValue* port = require_key(parsed, "server.port");
    expect_equal(std::get<std::int64_t>(port->value), static_cast<std::int64_t>(8080), "port should parse as integer");

    const TomlValue* manage_signals = require_key(parsed, "server.manage_signals");
    expect_true(std::get<bool>(manage_signals->value), "manage_signals should parse as bool");

    const TomlValue* level = require_key(parsed, "logger.level");
    expect_equal(std::get<std::string>(level->value), std::string("Trace"), "level should parse as string");

    const TomlValue* dir = require_key(parsed, "logger.dir");
    expect_equal(std::get<std::string>(dir->value), std::string("runtime/logs"), "dir should parse as string");
}

void test_parse_duplicate_key_rejected() {
    const std::string text = "[server]\nport = 1\nport = 2\n";
    const TomlParseResult parsed = nebula::common::parse_toml(text);

    expect_true(!parsed.ok, "duplicate key should fail");
    expect_equal(parsed.error_line, static_cast<std::size_t>(3), "duplicate key should report line");
    expect_equal(parsed.error, std::string("duplicate_key"), "duplicate key should report fixed error");
}

void test_parse_invalid_key_rejected() {
    const std::string text = "[server]\nmax.body = 1\n";
    const TomlParseResult parsed = nebula::common::parse_toml(text);

    expect_true(!parsed.ok, "invalid key should fail");
    expect_equal(parsed.error_line, static_cast<std::size_t>(2), "invalid key should report line");
    expect_equal(parsed.error, std::string("invalid_key"), "invalid key should report fixed error");
}

void test_parse_unsupported_value_type_rejected() {
    const std::string text = "[server]\nport = 1.5\n";
    const TomlParseResult parsed = nebula::common::parse_toml(text);

    expect_true(!parsed.ok, "unsupported value type should fail");
    expect_equal(parsed.error_line, static_cast<std::size_t>(2), "unsupported value type should report line");
    expect_contains(parsed.error, "invalid_integer", "invalid float should report integer parse error");
}

void test_parse_unterminated_string_rejected() {
    const std::string text = "[logger]\nlevel = \"trace\n";
    const TomlParseResult parsed = nebula::common::parse_toml(text);

    expect_true(!parsed.ok, "unterminated string should fail");
    expect_equal(parsed.error_line, static_cast<std::size_t>(2), "unterminated string should report line");
    expect_equal(parsed.error, std::string("unterminated_string"), "unterminated string should have fixed error");
}

int run_toml_parser_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"parse sections values comments", test_parse_sections_values_comments},
        {"parse duplicate key rejected", test_parse_duplicate_key_rejected},
        {"parse invalid key rejected", test_parse_invalid_key_rejected},
        {"parse unsupported value type rejected", test_parse_unsupported_value_type_rejected},
        {"parse unterminated string rejected", test_parse_unterminated_string_rejected},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_toml_parser_tests);
}
