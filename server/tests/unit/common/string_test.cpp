#include "nebula/common/base/string.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_trim_ascii_empty_input() {
    test::expect_equal(common::trim_ascii(""), std::string(""), "trim of empty text should be empty");
}

void test_trim_ascii_all_whitespace() {
    test::expect_equal(common::trim_ascii(" \t\r\n "), std::string(""), "trim should drop all surrounding whitespace");
}

void test_trim_ascii_leading_and_trailing_whitespace() {
    test::expect_equal(common::trim_ascii("  hello world\t"), std::string("hello world"),
                       "trim should remove both leading and trailing whitespace");
}

void test_trim_ascii_preserves_inner_whitespace() {
    test::expect_equal(common::trim_ascii(" a  b \t c "), std::string("a  b \t c"),
                       "trim should preserve inner whitespace");
}

void test_parse_number_valid_integer() {
    const auto value = common::parse_number<std::int64_t>("123456");
    test::expect_true(value.has_value(), "integer should parse");
    test::expect_equal(*value, std::int64_t{123456}, "parsed integer should match");
}

void test_parse_number_valid_negative_integer() {
    const auto value = common::parse_number<std::int64_t>("-42");
    test::expect_true(value.has_value(), "negative integer should parse");
    test::expect_equal(*value, std::int64_t{-42}, "parsed negative integer should match");
}

void test_parse_number_valid_double() {
    const auto value = common::parse_number<double>("6.25e1");
    test::expect_true(value.has_value(), "double should parse");
    test::expect_true(*value > 62.49 && *value < 62.51, "parsed double should match");
}

void test_parse_number_rejects_empty_input() {
    const auto value = common::parse_number<std::int64_t>("");
    test::expect_true(!value.has_value(), "empty input should fail");
    test::expect_equal(value.error(), common::ParseNumberError::Empty, "empty input should return empty error");
}

void test_parse_number_rejects_trailing_characters() {
    const auto value = common::parse_number<std::int64_t>("123abc");
    test::expect_true(!value.has_value(), "trailing characters should fail");
    test::expect_equal(value.error(), common::ParseNumberError::Invalid,
                       "trailing characters should return invalid error");
}

void test_parse_number_rejects_out_of_range_integer() {
    const auto value = common::parse_number<std::int64_t>("9223372036854775808");
    test::expect_true(!value.has_value(), "out-of-range value should fail");
    test::expect_equal(value.error(), common::ParseNumberError::OutOfRange,
                       "out-of-range value should return range error");
}

void test_parse_number_rejects_out_of_range_double() {
    const auto value = common::parse_number<double>("1e309");
    test::expect_true(!value.has_value(), "out-of-range double should fail");
    test::expect_equal(value.error(), common::ParseNumberError::OutOfRange,
                       "out-of-range double should return range error");
}

int run_string_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"trim ascii empty input", test_trim_ascii_empty_input},
        {"trim ascii all whitespace", test_trim_ascii_all_whitespace},
        {"trim ascii leading and trailing whitespace", test_trim_ascii_leading_and_trailing_whitespace},
        {"trim ascii preserves inner whitespace", test_trim_ascii_preserves_inner_whitespace},
        {"parse number valid integer", test_parse_number_valid_integer},
        {"parse number valid negative integer", test_parse_number_valid_negative_integer},
        {"parse number valid double", test_parse_number_valid_double},
        {"parse number rejects empty input", test_parse_number_rejects_empty_input},
        {"parse number rejects trailing characters", test_parse_number_rejects_trailing_characters},
        {"parse number rejects out of range integer", test_parse_number_rejects_out_of_range_integer},
        {"parse number rejects out of range double", test_parse_number_rejects_out_of_range_double},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_string_tests);
}
