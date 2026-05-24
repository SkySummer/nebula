#include "nebula/common/codec/base64.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_base64_string_round_trip() {
    const std::string input = "nebula auth";
    const std::string encoded = common::base64_encode(input);
    const auto decoded = common::base64_decode_to_string(encoded);
    if (!decoded.has_value()) {
        test::fail("base64 decode should succeed for encoded string");
    }
    test::expect_equal(*decoded, input, "base64 decoded string should match original");
}

void test_base64_known_vectors_with_padding() {
    test::expect_equal(common::base64_encode("f"), std::string("Zg=="), "single char should be padded");
    test::expect_equal(common::base64_encode("fo"), std::string("Zm8="), "two chars should be padded");
    test::expect_equal(common::base64_encode("foo"), std::string("Zm9v"), "three chars should not need padding");

    const std::vector<std::byte> bytes = {std::byte{0xFB}, std::byte{0xFF}};
    test::expect_equal(common::base64_encode(bytes), std::string("+/8="),
                       "standard alphabet should include plus slash");

    const auto decoded = common::base64_decode_to_string("Zm9v");
    if (!decoded.has_value()) {
        test::fail("known vector decode should succeed");
    }
    test::expect_equal(*decoded, std::string("foo"), "base64 decoded known vector should match");

    const auto alphabet_bytes = common::base64_decode_to_bytes("+/8=");
    if (!alphabet_bytes.has_value()) {
        test::fail("standard alphabet decode should accept plus slash");
    }
    test::expect_equal(*alphabet_bytes, bytes, "standard alphabet decode should map plus slash via lookup table");
}

void test_base64_decode_rejects_invalid_padding_or_chars() {
    test::expect_true(!common::base64_decode_to_string("a").has_value(), "size mod 4 equals 1 should fail");
    test::expect_true(!common::base64_decode_to_bytes("abc*").has_value(), "non base64 char should fail");
    test::expect_true(!common::base64_decode_to_string("=abc").has_value(), "leading padding should fail");
    test::expect_true(!common::base64_decode_to_string("ab=c").has_value(), "middle padding should fail");

    test::expect_equal(common::base64_decode_to_string("a").error(), common::Base64DecodeError::InvalidLength,
                       "invalid base64 length should return stable error");
    test::expect_equal(common::base64_decode_to_bytes("abc*").error(), common::Base64DecodeError::InvalidCharacter,
                       "invalid base64 character should return stable error");
    test::expect_equal(common::base64_decode_to_string("=abc").error(), common::Base64DecodeError::InvalidPadding,
                       "invalid base64 padding should return stable error");
}

void test_base64url_string_round_trip() {
    const std::string input = "nebula_test.jwt-token_123";
    const std::string encoded = common::base64url_encode(input);
    const auto decoded = common::base64url_decode_to_string(encoded);
    if (!decoded.has_value()) {
        test::fail("decode should succeed for encoded string");
    }
    test::expect_equal(*decoded, input, "decoded string should match original");
}

void test_base64url_bytes_round_trip() {
    const std::vector<std::byte> input = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFE}, std::byte{0xFF},
    };
    const std::string encoded = common::base64url_encode(input);
    const auto decoded = common::base64url_decode_to_bytes(encoded);
    if (!decoded.has_value()) {
        test::fail("decode should succeed for encoded bytes");
    }
    test::expect_equal(*decoded, input, "decoded bytes should match original");
}

void test_base64url_known_vectors_without_padding() {
    test::expect_equal(common::base64url_encode("f"), std::string("Zg"), "single char should be unpadded");
    test::expect_equal(common::base64url_encode("fo"), std::string("Zm8"), "two chars should be unpadded");
    test::expect_equal(common::base64url_encode("foo"), std::string("Zm9v"), "three chars should be unpadded");

    const auto decoded = common::base64url_decode_to_string("Zm9v");
    if (!decoded.has_value()) {
        test::fail("known vector decode should succeed");
    }
    test::expect_equal(*decoded, std::string("foo"), "decoded known vector should match");

    const std::vector<std::byte> bytes = {std::byte{0xFB}, std::byte{0xFF}};
    const auto alphabet_bytes = common::base64url_decode_to_bytes("-_8");
    if (!alphabet_bytes.has_value()) {
        test::fail("base64url decode should accept dash underscore");
    }
    test::expect_equal(*alphabet_bytes, bytes, "base64url decode should map dash underscore via lookup table");
}

void test_base64url_decode_rejects_invalid_input() {
    test::expect_true(!common::base64url_decode_to_string("a").has_value(), "size mod 4 equals 1 should fail");
    test::expect_true(!common::base64url_decode_to_bytes("abc*").has_value(), "non base64url char should fail");
    test::expect_true(!common::base64url_decode_to_string("Zg==").has_value(), "padding should fail for base64url");

    test::expect_equal(common::base64url_decode_to_string("a").error(), common::Base64DecodeError::InvalidLength,
                       "invalid base64url length should return stable error");
    test::expect_equal(common::base64url_decode_to_bytes("abc*").error(), common::Base64DecodeError::InvalidCharacter,
                       "invalid base64url character should return stable error");
    test::expect_equal(common::base64url_decode_to_string("Zg==").error(), common::Base64DecodeError::InvalidPadding,
                       "invalid base64url padding should return stable error");
}

int run_base64_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"base64 string round trip", test_base64_string_round_trip},
        {"base64 known vectors with padding", test_base64_known_vectors_with_padding},
        {"base64 decode rejects invalid padding or chars", test_base64_decode_rejects_invalid_padding_or_chars},
        {"base64url string round trip", test_base64url_string_round_trip},
        {"base64url bytes round trip", test_base64url_bytes_round_trip},
        {"base64url known vectors without padding", test_base64url_known_vectors_without_padding},
        {"base64url decode rejects invalid input", test_base64url_decode_rejects_invalid_input},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_base64_tests);
}
