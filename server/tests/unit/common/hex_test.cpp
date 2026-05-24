#include "nebula/common/base/hex.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_bytes_to_hex_known_vector() {
    const std::array<unsigned char, 4> bytes = {0x00U, 0x01U, 0xAFU, 0xFFU};
    test::expect_equal(common::bytes_to_hex(bytes), std::string("0001afff"), "bytes_to_hex should use lowercase hex");
}

void test_is_valid_lower_hex_token_accepts_exact_lowercase_hex() {
    test::expect_true(common::is_valid_lower_hex_token("00ff1a2b", std::size_t{8}),
                      "lowercase hex token with exact length should be accepted");
}

void test_is_valid_lower_hex_token_rejects_wrong_length_or_chars() {
    test::expect_true(!common::is_valid_lower_hex_token("00ff", std::size_t{8}), "short token should be rejected");
    test::expect_true(!common::is_valid_lower_hex_token("00FF1A2B", std::size_t{8}),
                      "uppercase hex token should be rejected");
    test::expect_true(!common::is_valid_lower_hex_token("00fg1a2b", std::size_t{8}),
                      "non-hex token should be rejected");
}

int run_hex_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"bytes to hex known vector", test_bytes_to_hex_known_vector},
        {"is valid lower hex token accepts exact lowercase hex",
         test_is_valid_lower_hex_token_accepts_exact_lowercase_hex},
        {"is valid lower hex token rejects wrong length or chars",
         test_is_valid_lower_hex_token_rejects_wrong_length_or_chars},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_hex_tests);
}
