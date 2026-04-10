#include "nebula/common/base64.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::base64_decode_to_bytes;
using nebula::common::base64_decode_to_string;
using nebula::common::base64_encode;
using nebula::common::base64url_decode_to_bytes;
using nebula::common::base64url_decode_to_string;
using nebula::common::base64url_encode;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::fail;

void test_base64_string_round_trip() {
    const std::string input = "nebula auth";
    const std::string encoded = base64_encode(input);
    const std::optional<std::string> decoded = base64_decode_to_string(encoded);
    expect_true(decoded.has_value(), "base64 decode should succeed for encoded string");
    if (!decoded.has_value()) {
        fail("base64 decode should succeed for encoded string");
    }
    expect_equal(decoded.value(), input, "base64 decoded string should match original");
}

void test_base64_known_vectors_with_padding() {
    expect_equal(base64_encode("f"), std::string("Zg=="), "single char should be padded");
    expect_equal(base64_encode("fo"), std::string("Zm8="), "two chars should be padded");
    expect_equal(base64_encode("foo"), std::string("Zm9v"), "three chars should not need padding");

    const std::vector<std::uint8_t> bytes = {0xFBU, 0xFFU};
    expect_equal(base64_encode(bytes), std::string("+/8="), "standard alphabet should include plus slash");

    const std::optional<std::string> decoded = base64_decode_to_string("Zm9v");
    expect_true(decoded.has_value(), "known vector decode should succeed");
    if (!decoded.has_value()) {
        fail("known vector decode should succeed");
    }
    expect_equal(decoded.value(), std::string("foo"), "base64 decoded known vector should match");
}

void test_base64_decode_rejects_invalid_padding_or_chars() {
    expect_true(!base64_decode_to_string("a").has_value(), "size mod 4 equals 1 should fail");
    expect_true(!base64_decode_to_bytes("abc*").has_value(), "non base64 char should fail");
    expect_true(!base64_decode_to_string("=abc").has_value(), "leading padding should fail");
    expect_true(!base64_decode_to_string("ab=c").has_value(), "middle padding should fail");
}

void test_base64url_string_round_trip() {
    const std::string input = "nebula_test.jwt-token_123";
    const std::string encoded = base64url_encode(input);
    const std::optional<std::string> decoded = base64url_decode_to_string(encoded);
    expect_true(decoded.has_value(), "decode should succeed for encoded string");
    if (!decoded.has_value()) {
        fail("decode should succeed for encoded string");
    }
    expect_equal(decoded.value(), input, "decoded string should match original");
}

void test_base64url_bytes_round_trip() {
    const std::vector<std::uint8_t> input = {0x00U, 0x01U, 0x7FU, 0x80U, 0xFEU, 0xFFU};
    const std::string encoded = base64url_encode(input);
    const std::optional<std::vector<std::uint8_t>> decoded = base64url_decode_to_bytes(encoded);
    expect_true(decoded.has_value(), "decode should succeed for encoded bytes");
    if (!decoded.has_value()) {
        fail("decode should succeed for encoded bytes");
    }
    expect_equal(decoded.value(), input, "decoded bytes should match original");
}

void test_base64url_known_vectors_without_padding() {
    expect_equal(base64url_encode("f"), std::string("Zg"), "single char should be unpadded");
    expect_equal(base64url_encode("fo"), std::string("Zm8"), "two chars should be unpadded");
    expect_equal(base64url_encode("foo"), std::string("Zm9v"), "three chars should be unpadded");

    const std::optional<std::string> decoded = base64url_decode_to_string("Zm9v");
    expect_true(decoded.has_value(), "known vector decode should succeed");
    if (!decoded.has_value()) {
        fail("known vector decode should succeed");
    }
    expect_equal(decoded.value(), std::string("foo"), "decoded known vector should match");
}

void test_base64url_decode_rejects_invalid_input() {
    expect_true(!base64url_decode_to_string("a").has_value(), "size mod 4 equals 1 should fail");
    expect_true(!base64url_decode_to_bytes("abc*").has_value(), "non base64url char should fail");
    expect_true(!base64url_decode_to_string("Zg==").has_value(), "padding should fail for base64url");
}

int run_base64_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"base64 string round trip", test_base64_string_round_trip},
        {"base64 known vectors with padding", test_base64_known_vectors_with_padding},
        {"base64 decode rejects invalid padding or chars", test_base64_decode_rejects_invalid_padding_or_chars},
        {"base64url string round trip", test_base64url_string_round_trip},
        {"base64url bytes round trip", test_base64url_bytes_round_trip},
        {"base64url known vectors without padding", test_base64url_known_vectors_without_padding},
        {"base64url decode rejects invalid input", test_base64url_decode_rejects_invalid_input},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_base64_tests);
}
