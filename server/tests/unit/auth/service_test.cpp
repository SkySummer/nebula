#include "nebula/auth/application/service.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/auth/infra/jwt_service.hpp"
#include "nebula/auth/infra/password_hasher.hpp"
#include "nebula/auth/repository/repository.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"

namespace {

using namespace nebula;

std::shared_ptr<nebula::auth::AuthRepository> setup_auth_repository() {
    const database::DatabaseConfig config = test::database::build_test_database_config();
    auto database_pool = test::database::create_database_pool(config);
    auto repository = std::make_shared<nebula::auth::AuthRepository>(database_pool);

    const bool schema_ready = repository->check_schema_ready();
    test::expect_true(schema_ready, "auth repository readiness check should succeed with valid test database");
    test::database::truncate_database_tables(config);
    return repository;
}

auth::AuthService make_auth_service(const std::shared_ptr<nebula::auth::AuthRepository>& repository) {
    return auth::AuthService(
        repository,
        auth::PasswordHasher(auth::PasswordHashConfig{.iterations = nebula::auth::kMinPasswordHashIterations}),
        auth::JwtService(
            auth::JwtConfig{.secret = test::to_byte_vector("auth-test-secret"), .access_token_ttl_s = 3600}));
}

template <typename Result>
void expect_auth_success(const Result& result, std::string_view message) {
    test::expect_true(result.has_value(), std::string(message));
}

template <typename Result>
void expect_auth_error(const Result& result, auth::AuthError error, std::string_view message) {
    test::expect_true(!result.has_value(), std::string(message));
    if (!result.has_value()) {
        test::expect_equal(result.error(), error, std::format("{} error code should match", message));
    }
}

void test_user_role_and_status_string_roundtrip() {
    test::expect_equal(nebula::auth::to_string(auth::UserRole::Owner), std::string_view("owner"),
                       "owner text should match");
    test::expect_equal(nebula::auth::to_string(auth::UserRole::Admin), std::string_view("admin"),
                       "admin text should match");
    test::expect_equal(nebula::auth::to_string(auth::UserRole::User), std::string_view("user"),
                       "user text should match");
    test::expect_equal(nebula::auth::parse_user_role("owner"), std::optional<auth::UserRole>(auth::UserRole::Owner),
                       "owner text should parse");
    test::expect_equal(nebula::auth::parse_user_status("disabled"),
                       std::optional<auth::UserStatus>(auth::UserStatus::Disabled), "disabled text should parse");
}

void test_username_validation_boundaries() {
    test::expect_true(!auth::AuthService::is_valid_username("ab"), "username shorter than 3 should be rejected");
    test::expect_true(auth::AuthService::is_valid_username("abc"), "username with length 3 should be accepted");
    test::expect_true(auth::AuthService::is_valid_username("Alice_01"), "alnum underscore username should be accepted");
    test::expect_true(!auth::AuthService::is_valid_username("alice-01"), "username with dash should be rejected");
    test::expect_true(!auth::AuthService::is_valid_username(std::string(33, 'a')),
                      "username longer than 32 should be rejected");
}

void test_password_validation_boundaries() {
    test::expect_true(!auth::AuthService::is_valid_password("short"), "password shorter than 8 should be rejected");
    test::expect_true(auth::AuthService::is_valid_password("password"), "password with length 8 should be accepted");
    test::expect_true(auth::AuthService::is_valid_password(std::string(72, 'a')),
                      "password with length 72 should be accepted");
    test::expect_true(!auth::AuthService::is_valid_password(std::string(73, 'a')),
                      "password longer than 72 should be rejected");
}

void test_password_hasher_hash_and_verify() {
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(auth::PasswordHashConfig{.iterations = nebula::auth::kMinPasswordHashIterations});
    const auto hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        nebula::test::fail("hash_password should produce hash");
    }
    test::expect_true(auth::PasswordHasher::verify_password(password, *hash),
                      "verify_password should accept correct password");
    test::expect_true(!auth::PasswordHasher::verify_password("wrong_password_123", *hash),
                      "verify_password should reject wrong password");
}

void test_password_hasher_verify_rejects_cost_amplification_payload() {
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(
        auth::PasswordHashConfig{.iterations = nebula::auth::kMinPasswordHashIterations, .derived_key_bytes = 32});
    const auto hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        nebula::test::fail("hash_password should produce hash");
    }

    auth::PasswordHashValue high_iterations_hash = *hash;
    high_iterations_hash.iterations = nebula::auth::kMaxPasswordHashIterations + 1U;
    test::expect_true(!auth::PasswordHasher::verify_password(password, high_iterations_hash),
                      "verify_password should reject iterations above allowed max");

    auth::PasswordHashValue oversized_derived_hash = *hash;
    oversized_derived_hash.derived_key = nebula::common::base64url_encode(
        std::vector<std::byte>(nebula::auth::kMaxPasswordHashDerivedKeyBytes + 1U, std::byte{7}));
    test::expect_true(!auth::PasswordHasher::verify_password(password, oversized_derived_hash),
                      "verify_password should reject derived key length above allowed max");
}

void test_auth_service_register_assigns_owner_then_user() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto first = auth_service.register_user("Alice_1", "password_123");
    expect_auth_success(first, "first register_user should succeed");
    test::expect_equal(first->user.role, auth::UserRole::Owner, "first user should become owner");
    test::expect_equal(first->user.status, auth::UserStatus::Active, "first user should default to active");

    const auto second = auth_service.register_user("Bob_1", "password_123");
    expect_auth_success(second, "second register_user should succeed");
    test::expect_equal(second->user.role, auth::UserRole::User, "second user should default to user role");

    const auto duplicate = auth_service.register_user("Alice_1", "password_123");
    expect_auth_error(duplicate, auth::AuthError::UserAlreadyExists, "duplicate register_user should be rejected");

    const auto login = auth_service.login_user("Bob_1", "password_123");
    expect_auth_success(login, "login should succeed with correct credentials");
    test::expect_equal(login->user.role, auth::UserRole::User, "login should return persisted role");

    const auto authenticated = auth_service.authenticate_access_token(login->access_token);
    expect_auth_success(authenticated, "authenticate_access_token should verify token");
    test::expect_equal(authenticated->user.role, auth::UserRole::User, "verified token should expose role");
    test::expect_equal(authenticated->user.status, auth::UserStatus::Active, "verified token should expose status");
}

void test_auth_service_rejects_disabled_login_and_token() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto owner = auth_service.register_user("Alice_1", "password_123");
    const auto user = auth_service.register_user("Bob_1", "password_123");
    expect_auth_success(owner, "owner register should succeed");
    expect_auth_success(user, "user register should succeed");

    const auto owner_auth = auth_service.authenticate_access_token(owner->access_token);
    expect_auth_success(owner_auth, "owner token should authenticate");

    const auto disabled =
        auth_service.update_user(owner_auth->user, user->user.user_id, std::nullopt, auth::UserStatus::Disabled);
    expect_auth_success(disabled, "owner should disable user");
    test::expect_equal(disabled->status, auth::UserStatus::Disabled, "updated user should be disabled");

    const auto login = auth_service.login_user("Bob_1", "password_123");
    expect_auth_error(login, auth::AuthError::UserDisabled, "disabled user should not be able to login");

    const auto authenticated = auth_service.authenticate_access_token(user->access_token);
    expect_auth_error(authenticated, auth::AuthError::UserDisabled,
                      "disabled user existing token should fail on next request");
}

void test_auth_service_change_password_rotates_token() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto registered = auth_service.register_user("Alice_1", "password_123");
    expect_auth_success(registered, "register should succeed");

    const auto actor = auth_service.authenticate_access_token(registered->access_token);
    expect_auth_success(actor, "fresh token should authenticate");

    const auto changed = auth_service.change_password(actor->user, "password_123", "password_456");
    expect_auth_success(changed, "change_password should succeed");
    test::expect_true(!changed->access_token.empty(), "change_password should issue new access token");

    const auto old_password_login = auth_service.login_user("Alice_1", "password_123");
    expect_auth_error(old_password_login, auth::AuthError::InvalidCredentials,
                      "old password should stop working after password change");

    const auto new_password_login = auth_service.login_user("Alice_1", "password_456");
    expect_auth_success(new_password_login, "new password should work after password change");

    const auto old_token = auth_service.authenticate_access_token(registered->access_token);
    expect_auth_error(old_token, auth::AuthError::TokenInvalid, "old token should be invalid after password change");

    const auto new_token = auth_service.authenticate_access_token(changed->access_token);
    expect_auth_success(new_token, "new token should authenticate after password change");
}

void test_auth_service_multiple_password_changes_keep_latest_login_token_valid() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto registered = auth_service.register_user("Alice_1", "password_123");
    expect_auth_success(registered, "register should succeed");
    const auto actor = auth_service.authenticate_access_token(registered->access_token);
    expect_auth_success(actor, "registered token should authenticate");

    const auto changed_once = auth_service.change_password(actor->user, "password_123", "password_456");
    expect_auth_success(changed_once, "first change_password should succeed");

    const auto actor_after_first = auth_service.authenticate_access_token(changed_once->access_token);
    expect_auth_success(actor_after_first, "first rotated token should authenticate");

    const auto changed_twice = auth_service.change_password(actor_after_first->user, "password_456", "password_789");
    expect_auth_success(changed_twice, "second change_password should succeed");

    const auto login_after_changes = auth_service.login_user("Alice_1", "password_789");
    expect_auth_success(login_after_changes,
                        "login with latest password should succeed after repeated password changes");

    const auto latest_login_token = auth_service.authenticate_access_token(login_after_changes->access_token);
    expect_auth_success(latest_login_token, "latest login token should stay valid after repeated password changes");
}

void test_auth_service_admin_cannot_manage_owner_or_grant_owner() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto owner = auth_service.register_user("Alice_1", "password_123");
    const auto user = auth_service.register_user("Bob_1", "password_123");
    expect_auth_success(owner, "owner register should succeed");
    expect_auth_success(user, "user register should succeed");

    const auto owner_actor = auth_service.authenticate_access_token(owner->access_token);
    expect_auth_success(owner_actor, "owner token should authenticate");

    const auto promoted_admin =
        auth_service.update_user(owner_actor->user, user->user.user_id, auth::UserRole::Admin, std::nullopt);
    expect_auth_success(promoted_admin, "owner should promote user to admin");

    const auto admin_login = auth_service.login_user("Bob_1", "password_123");
    expect_auth_success(admin_login, "admin login should succeed");
    const auto admin_actor = auth_service.authenticate_access_token(admin_login->access_token);
    expect_auth_success(admin_actor, "admin token should authenticate");

    const auto admin_disable_owner =
        auth_service.update_user(admin_actor->user, owner->user.user_id, std::nullopt, auth::UserStatus::Disabled);
    expect_auth_error(admin_disable_owner, auth::AuthError::Forbidden, "admin should not be allowed to manage owner");

    const auto admin_promote_owner =
        auth_service.update_user(admin_actor->user, admin_actor->user.user_id, auth::UserRole::Owner, std::nullopt);
    expect_auth_error(admin_promote_owner, auth::AuthError::Forbidden,
                      "admin should not be allowed to grant owner role");
}

void test_auth_service_last_owner_guard_and_user_list_permissions() {
    auto repository = setup_auth_repository();
    auth::AuthService auth_service = make_auth_service(repository);

    const auto owner = auth_service.register_user("Alice_1", "password_123");
    const auto user = auth_service.register_user("Bob_1", "password_123");
    expect_auth_success(owner, "owner register should succeed");
    expect_auth_success(user, "user register should succeed");

    const auto owner_actor = auth_service.authenticate_access_token(owner->access_token);
    const auto user_actor = auth_service.authenticate_access_token(user->access_token);
    expect_auth_success(owner_actor, "owner token should authenticate");
    expect_auth_success(user_actor, "user token should authenticate");

    const auto owner_list = auth_service.list_users(owner_actor->user, 50, 0);
    expect_auth_success(owner_list, "owner should be allowed to list users");
    test::expect_equal(static_cast<std::int64_t>(owner_list->users.size()), std::int64_t{2},
                       "list_users should return both users");

    const auto user_list = auth_service.list_users(user_actor->user, 50, 0);
    expect_auth_error(user_list, auth::AuthError::Forbidden, "plain user should not be allowed to list users");

    const auto disable_last_owner =
        auth_service.update_user(owner_actor->user, owner->user.user_id, std::nullopt, auth::UserStatus::Disabled);
    expect_auth_error(disable_last_owner, auth::AuthError::LastOwnerRequired,
                      "last active owner should not be disableable");
}

int run_service_tests() {
    test::database::require_database_test_env();

    const std::vector<nebula::test::TestCase> tests = {
        {"user role and status string roundtrip", test_user_role_and_status_string_roundtrip},
        {"username validation boundaries", test_username_validation_boundaries},
        {"password validation boundaries", test_password_validation_boundaries},
        {"password hasher hash and verify", test_password_hasher_hash_and_verify},
        {"password hasher verify rejects cost amplification payload",
         test_password_hasher_verify_rejects_cost_amplification_payload},
        {"auth service register assigns owner then user", test_auth_service_register_assigns_owner_then_user},
        {"auth service rejects disabled login and token", test_auth_service_rejects_disabled_login_and_token},
        {"auth service change password rotates token", test_auth_service_change_password_rotates_token},
        {"auth service multiple password changes keep latest login token valid",
         test_auth_service_multiple_password_changes_keep_latest_login_token_valid},
        {"auth service admin cannot manage owner or grant owner",
         test_auth_service_admin_cannot_manage_owner_or_grant_owner},
        {"auth service last owner guard and user list permissions",
         test_auth_service_last_owner_guard_and_user_list_permissions},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_service_tests);
}
