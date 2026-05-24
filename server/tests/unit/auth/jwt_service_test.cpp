#include "nebula/auth/infra/jwt_service.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula/common/platform/time.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

std::optional<std::string> hmac_sha256(std::span<const std::byte> key, std::string_view data) {
    unsigned int digest_len = 0;
    std::vector<unsigned char> digest(static_cast<std::size_t>(::EVP_MD_size(::EVP_sha256())), 0U);
    const std::vector<unsigned char> data_bytes(data.begin(), data.end());
    unsigned char* digest_ptr = ::HMAC(::EVP_sha256(), key.data(), static_cast<int>(key.size()), data_bytes.data(),
                                       data_bytes.size(), digest.data(), &digest_len);
    if (digest_ptr == nullptr) {
        return std::nullopt;
    }

    std::string digest_text(digest_len, '\0');
    for (std::size_t idx = 0; idx < digest_text.size(); ++idx) {
        digest_text[idx] = static_cast<char>(digest[idx]);
    }
    return digest_text;
}

std::optional<std::string> make_signed_token(std::span<const std::byte> secret, std::string_view payload_json) {
    const std::string header = nebula::common::base64url_encode(R"({"alg":"HS256","typ":"JWT"})");
    const std::string payload = nebula::common::base64url_encode(payload_json);
    const std::string signing_input = std::format("{}.{}", header, payload);
    const std::optional<std::string> signature = hmac_sha256(secret, signing_input);
    if (!signature.has_value()) {
        return std::nullopt;
    }
    return std::format("{}.{}", signing_input, nebula::common::base64url_encode(*signature));
}

void test_jwt_issue_verify_and_expire() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const auto token = jwt.issue_access_token(1, 3);
    if (!token.has_value()) {
        nebula::test::fail("issue_access_token should return token");
    }
    const std::string& token_value = *token;
    const auto claims = jwt.verify_access_token(token_value);
    test::expect_true(claims.has_value(), "verify_access_token should accept valid token");
    test::expect_equal(claims->user_id, std::int64_t{1}, "claims should include user_id");
    test::expect_equal(claims->token_version, std::int64_t{3}, "claims should include token version");
    test::expect_true(claims->issued_at_s > 0, "claims should include positive iat");
    test::expect_equal(claims->expires_at_s - claims->issued_at_s, std::int64_t{60}, "claims exp should match ttl");

    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> expired_token =
        make_signed_token(secret, std::format(R"({{"sub":"1","ver":3,"iat":{},"exp":{}}})", now_s - 120, now_s - 60));
    if (!expired_token.has_value()) {
        nebula::test::fail("expired test token should be generated");
    }
    const auto expired_claims = jwt.verify_access_token(*expired_token);
    test::expect_true(!expired_claims.has_value(), "verify_access_token should reject expired token");
    test::expect_equal(expired_claims.error(), auth::JwtVerifyError::Expired,
                       "verify_access_token should report expired token error");
}

void test_jwt_verify_rejects_tampered_token() {
    const auth::JwtService jwt(
        auth::JwtConfig{.secret = test::to_byte_vector("jwt-test-secret"), .access_token_ttl_s = 60});
    const auto token = jwt.issue_access_token(1, 1);
    if (!token.has_value()) {
        nebula::test::fail("issue_access_token should return token");
    }
    std::string tampered = *token;
    const std::size_t signature_offset = tampered.rfind('.');
    if (signature_offset == std::string::npos || (signature_offset + 1U) >= tampered.size()) {
        nebula::test::fail("issued token should contain signature segment");
    }
    tampered[signature_offset + 1U] = tampered[signature_offset + 1U] == 'A' ? 'B' : 'A';
    const auto claims = jwt.verify_access_token(tampered);
    test::expect_true(!claims.has_value(), "verify_access_token should reject tampered signature");
    test::expect_equal(claims.error(), auth::JwtVerifyError::Invalid,
                       "verify_access_token should report invalid token error");
}

void test_jwt_verify_rejects_token_with_non_positive_iat() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::optional<std::string> token = make_signed_token(secret, R"({"sub":"1","ver":1,"iat":0,"exp":60})");
    if (!token.has_value()) {
        nebula::test::fail("non-positive iat test token should be generated");
    }

    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(!claims.has_value(), "verify_access_token should reject token with non-positive iat");
    test::expect_equal(claims.error(), auth::JwtVerifyError::Invalid,
                       "verify_access_token should report invalid token for non-positive iat");
}

void test_jwt_verify_rejects_token_with_future_iat_beyond_clock_skew() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> token =
        make_signed_token(secret, std::format(R"({{"sub":"1","ver":1,"iat":{},"exp":{}}})",
                                              now_s + auth::kAccessTokenClockSkewSeconds + 1,
                                              now_s + auth::kAccessTokenClockSkewSeconds + 61));
    if (!token.has_value()) {
        nebula::test::fail("future iat test token should be generated");
    }

    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(!claims.has_value(), "verify_access_token should reject token with future iat beyond clock skew");
    test::expect_equal(claims.error(), auth::JwtVerifyError::Invalid,
                       "verify_access_token should report invalid token for future iat");
}

void test_jwt_verify_rejects_token_with_exp_before_iat() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> token =
        make_signed_token(secret, std::format(R"({{"sub":"1","ver":1,"iat":{},"exp":{}}})", now_s - 10, now_s - 11));
    if (!token.has_value()) {
        nebula::test::fail("exp before iat test token should be generated");
    }

    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(!claims.has_value(), "verify_access_token should reject token with exp before iat");
    test::expect_equal(claims.error(), auth::JwtVerifyError::Invalid,
                       "verify_access_token should report invalid token for exp before iat");
}

void test_jwt_verify_rejects_token_with_ttl_beyond_policy() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> token =
        make_signed_token(secret, std::format(R"({{"sub":"1","ver":1,"iat":{},"exp":{}}})", now_s - 10, now_s + 51));
    if (!token.has_value()) {
        nebula::test::fail("ttl beyond policy test token should be generated");
    }

    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(!claims.has_value(), "verify_access_token should reject token with ttl beyond policy");
    test::expect_equal(claims.error(), auth::JwtVerifyError::Invalid,
                       "verify_access_token should report invalid token for ttl beyond policy");
}

void test_jwt_constructor_rejects_ttl_below_minimum() {
    try {
        [[maybe_unused]] const auth::JwtService jwt(auth::JwtConfig{
            .secret = test::to_byte_vector("jwt-test-secret"),
            .access_token_ttl_s = auth::kMinAccessTokenTtlSeconds - 1,
        });
        nebula::test::fail("constructor should reject ttl below minimum");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("jwt_access_token_ttl_invalid"),
                           "constructor should report ttl range validation failure");
    }
}

void test_jwt_constructor_rejects_ttl_above_maximum() {
    try {
        [[maybe_unused]] const auth::JwtService jwt(auth::JwtConfig{
            .secret = test::to_byte_vector("jwt-test-secret"),
            .access_token_ttl_s = auth::kMaxAccessTokenTtlSeconds + 1,
        });
        nebula::test::fail("constructor should reject ttl above maximum");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("jwt_access_token_ttl_invalid"),
                           "constructor should report ttl range validation failure");
    }
}

void test_jwt_verify_accepts_token_with_extra_claims() {
    const std::vector<std::byte> secret = test::to_byte_vector("jwt-test-secret");
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> token = make_signed_token(
        secret, std::format(R"({{"sub":"1","ver":7,"iat":{},"exp":{},"jti":"trace-1","aud":"web-client"}})", now_s,
                            now_s + 60));
    if (!token.has_value()) {
        nebula::test::fail("test token signature should be generated");
    }
    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(claims.has_value(), "verify_access_token should accept payload with extra claims");
    test::expect_equal(claims->user_id, std::int64_t{1}, "claims should include user_id for extra claims token");
    test::expect_equal(claims->token_version, std::int64_t{7},
                       "claims should include token version for extra claims token");
    test::expect_equal(claims->issued_at_s, now_s, "claims should include iat for extra claims token");
    test::expect_equal(claims->expires_at_s, now_s + 60, "claims should include exp for extra claims token");
}

void test_jwt_issue_verify_with_binary_secret_bytes() {
    const std::vector<std::byte> secret = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}, std::byte{0x11},
        std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
        std::byte{0x88}, std::byte{0x99}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
        std::byte{0xEE}, std::byte{0xF0}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x81}, std::byte{0x92}, std::byte{0xA3},
        std::byte{0xB4}, std::byte{0xC5},
    };
    const auth::JwtService jwt(auth::JwtConfig{.secret = secret, .access_token_ttl_s = 60});
    const std::int64_t now_s = nebula::common::now_epoch_s();

    const std::optional<std::string> token =
        make_signed_token(secret, std::format(R"({{"sub":"9","ver":4,"iat":{},"exp":{}}})", now_s, now_s + 60));
    if (!token.has_value()) {
        nebula::test::fail("binary secret token should be generated");
    }

    const auto claims = jwt.verify_access_token(*token);
    test::expect_true(claims.has_value(), "verify_access_token should accept token signed with binary secret bytes");
    test::expect_equal(claims->user_id, std::int64_t{9}, "binary secret token should preserve user_id");
    test::expect_equal(claims->token_version, std::int64_t{4}, "binary secret token should preserve token version");
    test::expect_equal(claims->issued_at_s, now_s, "binary secret token should preserve iat");
    test::expect_equal(claims->expires_at_s, now_s + 60, "binary secret token should preserve exp");
}

int run_jwt_service_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"jwt issue verify and expire", test_jwt_issue_verify_and_expire},
        {"jwt verify rejects tampered token", test_jwt_verify_rejects_tampered_token},
        {"jwt verify rejects token with non-positive iat", test_jwt_verify_rejects_token_with_non_positive_iat},
        {"jwt verify rejects token with future iat beyond clock skew",
         test_jwt_verify_rejects_token_with_future_iat_beyond_clock_skew},
        {"jwt verify rejects token with exp before iat", test_jwt_verify_rejects_token_with_exp_before_iat},
        {"jwt verify rejects token with ttl beyond policy", test_jwt_verify_rejects_token_with_ttl_beyond_policy},
        {"jwt constructor rejects ttl below minimum", test_jwt_constructor_rejects_ttl_below_minimum},
        {"jwt constructor rejects ttl above maximum", test_jwt_constructor_rejects_ttl_above_maximum},
        {"jwt verify accepts token with extra claims", test_jwt_verify_accepts_token_with_extra_claims},
        {"jwt issue verify with binary secret bytes", test_jwt_issue_verify_with_binary_secret_bytes},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_jwt_service_tests);
}
