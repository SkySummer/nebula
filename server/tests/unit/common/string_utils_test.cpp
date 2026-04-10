#include "nebula/common/string_utils.hpp"

#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::trim_ascii;
using nebula::testsupport::expect_equal;

void test_trim_ascii_empty_input() {
    expect_equal(trim_ascii(""), std::string_view(""), "trim of empty text should be empty");
}

void test_trim_ascii_all_whitespace() {
    expect_equal(trim_ascii(" \t\r\n "), std::string_view(""), "trim should drop all surrounding whitespace");
}

void test_trim_ascii_leading_and_trailing_whitespace() {
    expect_equal(trim_ascii("  hello world\t"), std::string_view("hello world"),
                 "trim should remove both leading and trailing whitespace");
}

void test_trim_ascii_preserves_inner_whitespace() {
    expect_equal(trim_ascii(" a  b \t c "), std::string_view("a  b \t c"), "trim should preserve inner whitespace");
}

int run_string_utils_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"trim ascii empty input", test_trim_ascii_empty_input},
        {"trim ascii all whitespace", test_trim_ascii_all_whitespace},
        {"trim ascii leading and trailing whitespace", test_trim_ascii_leading_and_trailing_whitespace},
        {"trim ascii preserves inner whitespace", test_trim_ascii_preserves_inner_whitespace},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_string_utils_tests);
}
