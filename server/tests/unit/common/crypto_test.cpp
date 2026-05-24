#include "nebula/common/security/crypto.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_hmac_sha256_returns_sha256_sized_digest() {
    const std::optional<std::string> digest = common::hmac_sha256("nebula-key", "nebula-data");
    if (!digest.has_value()) {
        test::fail("hmac should succeed for known input");
    }
    test::expect_equal(digest->size(), std::size_t{32}, "sha256 hmac output should be 32 bytes");
}

void test_hmac_sha256_accepts_binary_key_bytes() {
    const std::vector<std::byte> binary_key = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80},
        std::byte{0xFF}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
    };
    const std::optional<std::string> digest =
        common::hmac_sha256(std::span<const std::byte>(binary_key), "nebula-data");
    if (!digest.has_value()) {
        test::fail("hmac should succeed for binary key bytes");
    }
    test::expect_equal(digest->size(), std::size_t{32}, "binary-key sha256 hmac output should be 32 bytes");
}

void test_generate_random_hex_token_128_returns_lowercase_hex() {
    const std::optional<std::string> token = common::generate_random_hex_token_128();
    if (!token.has_value()) {
        test::fail("random 128-bit token generation should succeed");
    }
    test::expect_equal(token->size(), nebula::common::kRandomHexToken128Chars,
                       "random 128-bit token should use fixed hex length");
    test::expect_true(common::is_valid_random_hex_token_128(*token), "random 128-bit token should be lowercase hex");
}

void test_is_valid_random_hex_token_128_rejects_wrong_length_or_chars() {
    test::expect_true(!common::is_valid_random_hex_token_128("00ff"), "short random hex token should be rejected");
    test::expect_true(!common::is_valid_random_hex_token_128("00FF00FF00FF00FF00FF00FF00FF00FF"),
                      "uppercase random hex token should be rejected");
    test::expect_true(!common::is_valid_random_hex_token_128("00ff00ff00ff00ff00ff00ff00ff00fg"),
                      "non-hex random token should be rejected");
}

int run_crypto_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"hmac sha256 returns sha256 sized digest", test_hmac_sha256_returns_sha256_sized_digest},
        {"hmac sha256 accepts binary key bytes", test_hmac_sha256_accepts_binary_key_bytes},
        {"generate random hex token 128 returns lowercase hex",
         test_generate_random_hex_token_128_returns_lowercase_hex},
        {"is valid random hex token 128 rejects wrong length or chars",
         test_is_valid_random_hex_token_128_rejects_wrong_length_or_chars},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_crypto_tests);
}
