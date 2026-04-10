#include "nebula/auth/auth_service.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/auth/jwt_service.hpp"
#include "nebula/auth/password_hash_limits.hpp"
#include "nebula/auth/password_hasher.hpp"
#include "nebula/common/base64.hpp"
#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula_tests/test_support.hpp"
#include "nebula_tests/test_support_database.hpp"

namespace {

using nebula::auth::AuthErrorCode;
using nebula::auth::AuthService;
using nebula::auth::JwtConfig;
using nebula::auth::JwtService;
using nebula::auth::kMinPasswordHashIterations;
using nebula::auth::PasswordHashConfig;
using nebula::auth::PasswordHasher;
using nebula::common::PostgresConnectionPool;
using nebula::common::PostgresConnectionPoolOptions;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::database::load_postgres_pool_test_options;
using nebula::testsupport::database::validate_database_test_env;
using nebula::user::check_user_schema_ready;

PostgresConnectionPoolOptions require_test_options() {
    const std::optional<PostgresConnectionPoolOptions> options = load_postgres_pool_test_options();
    if (!options.has_value()) {
        nebula::testsupport::fail("postgres test database env should be configured before running tests");
    }
    return options.value();
}

void truncate_users(const PostgresConnectionPoolOptions& options) {
    pqxx::connection connection(nebula::common::build_connection_info(options));
    pqxx::work tx(connection);
    tx.exec("TRUNCATE TABLE users RESTART IDENTITY");
    tx.exec("ALTER SEQUENCE users_user_id_seq RESTART WITH 1");
    tx.commit();
}

void initialize_database_user_repository() {
    const PostgresConnectionPoolOptions options = require_test_options();
    const PostgresConnectionPool::InitializeStatus status = PostgresConnectionPool::instance().initialize(options);
    expect_true(status == PostgresConnectionPool::InitializeStatus::Initialized ||
                    status == PostgresConnectionPool::InitializeStatus::AlreadyInitialized,
                "postgres pool should initialize with valid test database");

    const bool schema_ready = check_user_schema_ready();
    expect_true(schema_ready, "user schema check should succeed with valid test database");
    if (!schema_ready) {
        nebula::testsupport::fail("user schema check should succeed with valid test database");
    }

    truncate_users(options);
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1U;
    }
    return parts;
}

std::optional<std::uint64_t> parse_uint64(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

void test_username_validation_boundaries() {
    expect_true(!AuthService::is_valid_username("ab"), "username shorter than 3 should be rejected");
    expect_true(AuthService::is_valid_username("abc"), "username with length 3 should be accepted");
    expect_true(AuthService::is_valid_username("Alice_01"), "alnum underscore username should be accepted");
    expect_true(!AuthService::is_valid_username("alice-01"), "username with dash should be rejected");
    expect_true(!AuthService::is_valid_username(std::string(33, 'a')), "username longer than 32 should be rejected");
}

void test_password_validation_boundaries() {
    expect_true(!AuthService::is_valid_password("short"), "password shorter than 8 should be rejected");
    expect_true(AuthService::is_valid_password("password"), "password with length 8 should be accepted");
    expect_true(AuthService::is_valid_password(std::string(72, 'a')), "password with length 72 should be accepted");
    expect_true(!AuthService::is_valid_password(std::string(73, 'a')), "password longer than 72 should be rejected");
}

void test_password_hasher_hash_and_verify() {
    const std::string password = "strong_password_123";
    const PasswordHasher hasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations});
    const auto hash = hasher.hash_password(password);
    expect_true(hash.has_value(), "hash_password should produce hash");
    if (!hash.has_value()) {
        nebula::testsupport::fail("hash_password should produce hash");
    }
    const std::string& hash_value = *hash;
    expect_true(PasswordHasher::verify_password(password, hash_value),
                "verify_password should accept correct password");
    expect_true(!PasswordHasher::verify_password("wrong_password_123", hash_value),
                "verify_password should reject wrong password");
}

void test_password_hasher_verify_rejects_cost_amplification_payload() {
    const std::string password = "strong_password_123";
    const PasswordHasher hasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations, .derived_key_bytes = 32});
    const auto hash = hasher.hash_password(password);
    expect_true(hash.has_value(), "hash_password should produce hash");
    if (!hash.has_value()) {
        nebula::testsupport::fail("hash_password should produce hash");
    }

    const std::vector<std::string_view> parts = split(*hash, '$');
    expect_equal(parts.size(), static_cast<std::size_t>(4U), "encoded hash should have 4 parts");
    if (parts.size() != 4U) {
        nebula::testsupport::fail("encoded hash should have 4 parts");
    }

    const std::uint64_t high_iterations = static_cast<std::uint64_t>(nebula::auth::kMaxPasswordHashIterations) + 1U;
    const std::string high_iterations_hash = nebula::auth::format_password_hash(high_iterations, parts[2], parts[3]);
    expect_true(!PasswordHasher::verify_password(password, high_iterations_hash),
                "verify_password should reject iterations above allowed max");

    const std::optional<std::uint64_t> iterations = parse_uint64(parts[1]);
    expect_true(iterations.has_value(), "encoded hash iterations should be numeric");
    if (!iterations.has_value()) {
        nebula::testsupport::fail("encoded hash iterations should be numeric");
    }

    const std::string oversized_derived_hash =
        nebula::auth::format_password_hash(*iterations, parts[2],
                                           nebula::common::base64url_encode(std::vector<std::uint8_t>(
                                               nebula::auth::kMaxPasswordHashDerivedKeyBytes + 1U, 7U)));
    expect_true(!PasswordHasher::verify_password(password, oversized_derived_hash),
                "verify_password should reject derived key length above allowed max");
}

void test_auth_service_register_login_authenticate() {
    initialize_database_user_repository();
    AuthService auth_service(PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
                             JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));

    const auto registered = auth_service.register_user("Alice_1", "password_123");
    expect_equal(registered.error, AuthErrorCode::Ok, "register_user should succeed with valid payload");
    expect_true(!registered.access_token.empty(), "register_user should issue access token");

    const auto duplicate = auth_service.register_user("Alice_1", "password_123");
    expect_equal(duplicate.error, AuthErrorCode::UsernameAlreadyExists,
                 "register_user should reject duplicate username");

    const auto login = auth_service.login("Alice_1", "password_123");
    expect_equal(login.error, AuthErrorCode::Ok, "login should succeed with correct credentials");
    expect_true(!login.access_token.empty(), "login should issue access token");

    const auto login_failed = auth_service.login("Alice_1", "password_124");
    expect_equal(login_failed.error, AuthErrorCode::InvalidCredentials,
                 "login should reject wrong credentials with stable error");

    const auto authenticated = auth_service.authenticate_access_token(login.access_token);
    expect_equal(authenticated.error, AuthErrorCode::Ok, "authenticate_access_token should verify token");
    expect_equal(authenticated.user.user_id, registered.user.user_id, "verified token should map back to same user");
}

void test_auth_service_rejects_missing_or_invalid_token() {
    initialize_database_user_repository();
    AuthService auth_service(PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
                             JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));

    const auto missing = auth_service.authenticate_access_token("");
    expect_equal(missing.error, AuthErrorCode::TokenMissing, "empty token should be rejected");

    const auto invalid = auth_service.authenticate_access_token("bad.token.value");
    expect_equal(invalid.error, AuthErrorCode::TokenInvalid, "malformed token should be rejected");
}

void test_auth_service_register_does_not_reserve_username_on_token_issue_failure() {
    initialize_database_user_repository();
    AuthService auth_service_with_bad_jwt(PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
                                          JwtService(JwtConfig{.secret = "", .access_token_ttl_s = 3600}));

    const auto failed = auth_service_with_bad_jwt.register_user("Alice_2", "password_123");
    expect_equal(failed.error, AuthErrorCode::InternalError, "register_user should fail when token issue fails");

    AuthService auth_service_with_good_jwt(
        PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
        JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));
    const auto retry = auth_service_with_good_jwt.register_user("Alice_2", "password_123");
    expect_equal(retry.error, AuthErrorCode::Ok,
                 "register_user retry should succeed because failed attempt should not persist user");
    expect_true(!retry.access_token.empty(), "register_user retry should issue access token");
}

void test_auth_service_register_duplicate_short_circuits_before_hashing() {
    initialize_database_user_repository();
    AuthService auth_service_with_good_hash(
        PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
        JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));
    const auto registered = auth_service_with_good_hash.register_user("Alice_3", "password_123");
    expect_equal(registered.error, AuthErrorCode::Ok, "first register_user should succeed");

    AuthService auth_service_with_invalid_hash_config(
        PasswordHasher(PasswordHashConfig{.iterations = 0, .salt_bytes = 16, .derived_key_bytes = 32}),
        JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));
    const auto duplicate = auth_service_with_invalid_hash_config.register_user("Alice_3", "password_123");
    expect_equal(duplicate.error, AuthErrorCode::UsernameAlreadyExists,
                 "duplicate register_user should short-circuit before hashing");
}

void test_auth_service_register_duplicate_short_circuits_before_token_issue() {
    initialize_database_user_repository();
    AuthService auth_service_with_good_jwt(
        PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
        JwtService(JwtConfig{.secret = "auth-test-secret", .access_token_ttl_s = 3600}));
    const auto registered = auth_service_with_good_jwt.register_user("Alice_4", "password_123");
    expect_equal(registered.error, AuthErrorCode::Ok, "first register_user should succeed");

    AuthService auth_service_with_bad_jwt(PasswordHasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations}),
                                          JwtService(JwtConfig{.secret = "", .access_token_ttl_s = 3600}));
    const auto duplicate = auth_service_with_bad_jwt.register_user("Alice_4", "password_123");
    expect_equal(duplicate.error, AuthErrorCode::UsernameAlreadyExists,
                 "duplicate register_user should short-circuit before token issue");
}

int run_auth_service_tests() {
    const std::optional<std::string> env_error = validate_database_test_env();
    if (env_error.has_value()) {
        std::cerr << "[SKIP] auth service test precheck skipped: error=" << *env_error << '\n';
        return nebula::testsupport::database::kDatabaseTestSkipReturnCode;
    }

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"username validation boundaries", test_username_validation_boundaries},
        {"password validation boundaries", test_password_validation_boundaries},
        {"password hasher hash and verify", test_password_hasher_hash_and_verify},
        {"password hasher verify rejects cost amplification payload",
         test_password_hasher_verify_rejects_cost_amplification_payload},
        {"auth service register login authenticate", test_auth_service_register_login_authenticate},
        {"auth service rejects missing or invalid token", test_auth_service_rejects_missing_or_invalid_token},
        {"auth service register does not reserve username on token issue failure",
         test_auth_service_register_does_not_reserve_username_on_token_issue_failure},
        {"auth service register duplicate short circuits before hashing",
         test_auth_service_register_duplicate_short_circuits_before_hashing},
        {"auth service register duplicate short circuits before token issue",
         test_auth_service_register_duplicate_short_circuits_before_token_issue},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_auth_service_tests);
}
