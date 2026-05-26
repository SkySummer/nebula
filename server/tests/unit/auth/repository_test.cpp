#include "nebula/auth/repository/repository.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/common/codec/base64.hpp"
#include "nebula/database/config.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"

namespace {

using namespace nebula;

std::optional<std::int64_t> fetch_user_quota_bytes(const database::DatabaseConfig& config, std::int64_t user_id) {
    pqxx::connection connection(database::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    const pqxx::result rows =
        tx.exec("SELECT quota_bytes FROM users WHERE user_id = $1::bigint LIMIT 1", pqxx::params{tx, user_id});
    if (rows.empty()) {
        return std::nullopt;
    }

    const pqxx::row row(rows.one_row());
    if (row[0].is_null()) {
        return std::nullopt;
    }
    return row[0].as<std::int64_t>(0);
}

std::string make_base64url_blob(std::size_t size_bytes, std::byte fill_byte) {
    return nebula::common::base64url_encode(std::vector<std::byte>(size_bytes, fill_byte));
}

auth::PasswordHashValue make_hash(std::string_view suffix) {
    const auto fill_byte = static_cast<std::byte>(suffix.empty() ? 0x11U : static_cast<unsigned char>(suffix.front()));
    return {
        .algorithm = "pbkdf2_sha256",
        .iterations = 120000,
        .salt = make_base64url_blob(16U, fill_byte),
        .derived_key = make_base64url_blob(32U, fill_byte),
    };
}

void test_postgres_schema_check_fails_when_connection_unreachable() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 1,
        .name = "invalid_db",
        .user = "invalid_user",
        .password = "invalid_password",
        .max_connections = 1,
        .connect_timeout_s = 1,
        .acquire_timeout_ms = 1000,
    };

    auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool == nullptr, "postgres pool create should fail on unreachable connection");

    bool threw = false;
    try {
        [[maybe_unused]] auth::AuthRepository repository(pool);
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    test::expect_true(threw, "auth repository should reject unavailable pool");
}

void test_postgres_repository_crud_and_owner_guard() {
    const database::DatabaseConfig config = test::database::build_test_database_config();

    auto pool = database::ConnectionPool::create(config);
    test::expect_true(pool != nullptr, "shared postgres pool should create with valid test database");
    auth::AuthRepository repository(pool);
    const bool schema_ready = repository.check_schema_ready();
    test::expect_true(schema_ready, "auth repository readiness check should succeed with valid test database");
    test::database::truncate_database_tables(config);

    const auto created_owner = repository.create_user("Alice_1", make_hash("1"), 1700000000);
    test::expect_true(created_owner.has_value(), "create_user should insert first user");
    test::expect_equal(created_owner->profile.user_id, std::int64_t{1}, "first created user should get user_id 1");
    test::expect_equal(created_owner->profile.role, auth::UserRole::Owner, "first created user should become owner");
    test::expect_equal(created_owner->profile.status, auth::UserStatus::Active,
                       "created user should default to active");
    test::expect_equal(created_owner->token_version, std::int64_t{1}, "created user should start with token version 1");

    const auto duplicate = repository.create_user("Alice_1", make_hash("duplicate"), 1700000010);
    test::expect_true(!duplicate.has_value(), "create_user should reject duplicate username");
    test::expect_equal(duplicate.error(), auth::UserCreateError::DuplicateUsername,
                       "create_user should reject duplicate username");

    const auto created_user = repository.create_user("Bob_1", make_hash("2"), 1700000020);
    test::expect_true(created_user.has_value(), "create_user should insert second user");
    test::expect_true(created_user->profile.user_id > created_owner->profile.user_id,
                      "later successful create_user should still receive a larger user_id");
    test::expect_equal(created_user->profile.role, auth::UserRole::User,
                       "second created user should default to user role");
    test::expect_equal(created_user->profile.status, auth::UserStatus::Active,
                       "second created user should default to active");

    const auto found_owner = repository.find_user_by_username("Alice_1");
    test::expect_true(found_owner.has_value(), "find_by_username should return found");
    test::expect_equal(found_owner->profile.role, auth::UserRole::Owner, "find_by_username should map owner role");
    test::expect_equal(found_owner->profile.status, auth::UserStatus::Active,
                       "find_by_username should map active status");

    const auto found_user = repository.find_user_by_id(created_user->profile.user_id);
    test::expect_true(found_user.has_value(), "find_by_user_id should return found");
    test::expect_equal(found_user->profile.username, std::string("Bob_1"), "find_by_user_id should map username");

    const auto updated_password = repository.update_password_hash(created_user->profile.user_id, make_hash("2b"), 2);
    test::expect_true(updated_password.has_value(), "update_password_hash should update existing user");
    test::expect_equal(updated_password->token_version, std::int64_t{2},
                       "update_password_hash should advance token version");
    test::expect_equal(updated_password->password_hash.salt, make_hash("2b").salt,
                       "update_password_hash should update stored hash");

    const auto listed = repository.list_users(1, 0);
    if (!listed.has_value()) {
        test::fail("list_users should succeed");
    }
    test::expect_equal(static_cast<std::int64_t>(listed->users.size()), std::int64_t{1},
                       "list_users should respect limit");
    test::expect_true(listed->has_more, "list_users should report has_more when extra rows exist");
    test::expect_equal(listed->users.front().user_id, std::int64_t{1}, "list_users should sort by user_id asc");

    const auto promoted_admin =
        repository.update_user(created_user->profile.user_id, auth::UserRole::Admin, std::nullopt);
    test::expect_true(promoted_admin.has_value(), "update_user should promote user to admin");
    test::expect_equal(promoted_admin->role, auth::UserRole::Admin, "updated user should persist admin role");

    const auto reject_last_owner =
        repository.update_user(created_owner->profile.user_id, auth::UserRole::User, std::nullopt);
    test::expect_true(!reject_last_owner.has_value(), "last active owner should not be demotable");
    test::expect_equal(reject_last_owner.error(), auth::UserRoleStatusUpdateError::LastOwnerRequired,
                       "last active owner should not be demotable");

    const auto promoted_owner =
        repository.update_user(created_user->profile.user_id, auth::UserRole::Owner, std::nullopt);
    test::expect_true(promoted_owner.has_value(), "second user should be promotable to owner");

    const auto disabled_first_owner =
        repository.update_user(created_owner->profile.user_id, std::nullopt, auth::UserStatus::Disabled);
    test::expect_true(disabled_first_owner.has_value(), "owner should be disableable when another active owner exists");
    test::expect_equal(disabled_first_owner->status, auth::UserStatus::Disabled,
                       "updated owner should persist disabled status");

    const auto quota_bytes = fetch_user_quota_bytes(config, created_owner->profile.user_id);
    if (!quota_bytes.has_value()) {
        nebula::test::fail("created user should persist default quota");
    }
    test::expect_equal(*quota_bytes, std::int64_t{21474836480}, "created user should use default 20 GB quota");

    const auto missing_username = repository.find_user_by_username("Unknown");
    test::expect_true(!missing_username.has_value(), "unknown username should return not_found");
    test::expect_equal(missing_username.error(), auth::UserFindError::NotFound,
                       "unknown username should return not_found");
    const auto missing_user_id = repository.find_user_by_id(999999);
    test::expect_true(!missing_user_id.has_value(), "unknown user id should return not_found");
    test::expect_equal(missing_user_id.error(), auth::UserFindError::NotFound,
                       "unknown user id should return not_found");
}

int run_repository_tests() {
    test::database::require_database_test_env();

    const std::vector<nebula::test::TestCase> tests = {
        {"postgres repository readiness check fails when connection unreachable",
         test_postgres_schema_check_fails_when_connection_unreachable},
        {"postgres repository crud and owner guard", test_postgres_repository_crud_and_owner_guard},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_repository_tests);
}
