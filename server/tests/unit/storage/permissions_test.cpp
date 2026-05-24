#include "nebula/storage/domain/permissions.hpp"

#include <vector>

#include "nebula/auth/domain/user.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_can_run_global_storage_gc_allows_owner_and_admin_only() {
    test::expect_true(nebula::storage::can_run_global_storage_gc(auth::UserRole::Owner), "owner should run storage gc");
    test::expect_true(nebula::storage::can_run_global_storage_gc(auth::UserRole::Admin), "admin should run storage gc");
    test::expect_true(!nebula::storage::can_run_global_storage_gc(auth::UserRole::User),
                      "regular user should not run storage gc");
}

int run_permissions_tests() {
    std::vector<nebula::test::TestCase> tests = {
        {"can_run_global_storage_gc allows owner and admin only",
         test_can_run_global_storage_gc_allows_owner_and_admin_only},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_permissions_tests);
}
