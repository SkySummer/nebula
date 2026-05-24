#include "nebula/auth/domain/permissions.hpp"

#include <optional>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

auth::UserProfile make_user(auth::UserRole role) {
    return auth::UserProfile{.user_id = 1, .username = "user", .role = role};
}

void test_can_read_users_allows_owner_and_admin_only() {
    test::expect_true(nebula::auth::can_read_users(make_user(auth::UserRole::Owner)), "owner should read users");
    test::expect_true(nebula::auth::can_read_users(make_user(auth::UserRole::Admin)), "admin should read users");
    test::expect_true(!nebula::auth::can_read_users(make_user(auth::UserRole::User)),
                      "regular user should not read users");
}

void test_can_manage_user_blocks_admin_from_owner_operations() {
    const auth::UserProfile admin = make_user(auth::UserRole::Admin);
    const auth::UserProfile owner = make_user(auth::UserRole::Owner);
    const auth::UserProfile user = make_user(auth::UserRole::User);

    test::expect_true(!nebula::auth::can_manage_user(admin, owner, std::nullopt),
                      "admin should not manage owner target");
    test::expect_true(!nebula::auth::can_manage_user(admin, user, auth::UserRole::Owner),
                      "admin should not grant owner role");
    test::expect_true(nebula::auth::can_manage_user(admin, user, auth::UserRole::Admin),
                      "admin should manage non-owner target without granting owner");
}

void test_can_manage_user_allows_owner_and_blocks_regular_user() {
    const auth::UserProfile owner = make_user(auth::UserRole::Owner);
    const auth::UserProfile admin = make_user(auth::UserRole::Admin);
    const auth::UserProfile user = make_user(auth::UserRole::User);

    test::expect_true(nebula::auth::can_manage_user(owner, admin, auth::UserRole::Owner),
                      "owner should manage any target and role");
    test::expect_true(!nebula::auth::can_manage_user(user, admin, std::nullopt),
                      "regular user should not manage other users");
}

int run_permissions_tests() {
    std::vector<nebula::test::TestCase> tests = {
        {"can_read_users allows owner and admin only", test_can_read_users_allows_owner_and_admin_only},
        {"can_manage_user blocks admin from owner operations", test_can_manage_user_blocks_admin_from_owner_operations},
        {"can_manage_user allows owner and blocks regular user",
         test_can_manage_user_allows_owner_and_blocks_regular_user},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_permissions_tests);
}
