#include "nebula/auth/jwt_service.hpp"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "nebula/common/base64.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::auth::JwtConfig;
using nebula::auth::JwtService;
using nebula::auth::JwtVerifyResult;
using nebula::auth::TokenClaims;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

std::optional<std::string> hmac_sha256(std::string_view key, std::string_view data) {
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

void test_jwt_issue_verify_and_expire() {
    const JwtService jwt(JwtConfig{.secret = "jwt-test-secret", .access_token_ttl_s = 60});
    const auto token = jwt.issue_access_token(1, 1000);
    expect_true(token.has_value(), "issue_access_token should return token");
    if (!token.has_value()) {
        nebula::testsupport::fail("issue_access_token should return token");
    }
    const std::string& token_value = *token;

    TokenClaims claims;
    expect_equal(jwt.verify_access_token(token_value, claims, 1020), JwtVerifyResult::Valid,
                 "verify_access_token should accept valid token");
    expect_equal(claims.user_id, static_cast<std::int64_t>(1), "claims should include user_id");
    expect_equal(claims.issued_at_s, static_cast<std::int64_t>(1000), "claims should include iat");
    expect_equal(claims.expires_at_s, static_cast<std::int64_t>(1060), "claims should include exp");

    expect_equal(jwt.verify_access_token(token_value, claims, 1060), JwtVerifyResult::Expired,
                 "verify_access_token should reject token at exp boundary");
    expect_equal(claims.user_id, static_cast<std::int64_t>(0), "expired token should clear claims user_id");
    expect_equal(claims.issued_at_s, static_cast<std::int64_t>(0), "expired token should clear claims iat");
    expect_equal(claims.expires_at_s, static_cast<std::int64_t>(0), "expired token should clear claims exp");
    expect_equal(jwt.verify_access_token(token_value, claims, 1061), JwtVerifyResult::Expired,
                 "verify_access_token should reject expired token");
    expect_equal(claims.user_id, static_cast<std::int64_t>(0), "expired token should keep claims cleared");
    expect_equal(claims.issued_at_s, static_cast<std::int64_t>(0), "expired token should keep claims iat cleared");
    expect_equal(claims.expires_at_s, static_cast<std::int64_t>(0), "expired token should keep claims exp cleared");
}

void test_jwt_verify_rejects_tampered_token() {
    const JwtService jwt(JwtConfig{.secret = "jwt-test-secret", .access_token_ttl_s = 60});
    const auto token = jwt.issue_access_token(1, 1000);
    expect_true(token.has_value(), "issue_access_token should return token");
    if (!token.has_value()) {
        nebula::testsupport::fail("issue_access_token should return token");
    }

    std::string tampered = *token;
    tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
    TokenClaims claims{.user_id = 42, .issued_at_s = 1, .expires_at_s = 2};
    expect_equal(jwt.verify_access_token(tampered, claims, 1020), JwtVerifyResult::Invalid,
                 "verify_access_token should reject tampered signature");
    expect_equal(claims.user_id, static_cast<std::int64_t>(0), "invalid token should clear claims user_id");
    expect_equal(claims.issued_at_s, static_cast<std::int64_t>(0), "invalid token should clear claims iat");
    expect_equal(claims.expires_at_s, static_cast<std::int64_t>(0), "invalid token should clear claims exp");
}

void test_jwt_issue_rejects_exp_overflow() {
    const JwtService jwt(
        JwtConfig{.secret = "jwt-test-secret", .access_token_ttl_s = std::numeric_limits<std::int64_t>::max()});
    const auto token = jwt.issue_access_token(1, std::numeric_limits<std::int64_t>::max());
    expect_true(!token.has_value(), "issue_access_token should reject exp overflow");
}

void test_jwt_verify_accepts_token_with_extra_claims() {
    constexpr std::string_view secret = "jwt-test-secret";
    const JwtService jwt(JwtConfig{.secret = std::string(secret), .access_token_ttl_s = 60});
    const std::string header = nebula::common::base64url_encode(R"({"alg":"HS256","typ":"JWT"})");
    const std::string payload_json = R"({"sub":"1","iat":1000,"exp":1060,"jti":"trace-1","aud":"web-client"})";
    const std::string payload = nebula::common::base64url_encode(payload_json);
    const std::string signing_input = header + "." + payload;

    const std::optional<std::string> signature = hmac_sha256(secret, signing_input);
    expect_true(signature.has_value(), "test token signature should be generated");
    if (!signature.has_value()) {
        nebula::testsupport::fail("test token signature should be generated");
    }

    const std::string token = signing_input + "." + nebula::common::base64url_encode(*signature);
    TokenClaims claims;
    expect_equal(jwt.verify_access_token(token, claims, 1020), JwtVerifyResult::Valid,
                 "verify_access_token should accept payload with extra claims");
    expect_equal(claims.user_id, static_cast<std::int64_t>(1), "claims should include user_id for extra claims token");
    expect_equal(claims.issued_at_s, static_cast<std::int64_t>(1000),
                 "claims should include iat for extra claims token");
    expect_equal(claims.expires_at_s, static_cast<std::int64_t>(1060),
                 "claims should include exp for extra claims token");
}

int run_jwt_service_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"jwt issue verify and expire", test_jwt_issue_verify_and_expire},
        {"jwt verify rejects tampered token", test_jwt_verify_rejects_tampered_token},
        {"jwt issue rejects exp overflow", test_jwt_issue_rejects_exp_overflow},
        {"jwt verify accepts token with extra claims", test_jwt_verify_accepts_token_with_extra_claims},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_jwt_service_tests);
}
