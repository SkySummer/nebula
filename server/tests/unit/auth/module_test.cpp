#include "nebula/auth/bootstrap/module.hpp"

#include <memory>
#include <vector>

#include "nebula/auth/bootstrap/config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"

namespace {

using namespace nebula;

void test_auth_module_rejects_missing_database_pool() {
    auto router = std::make_shared<http::Router>();
    const auth::AuthConfig config{};
    const auto module = auth::AuthModule::create(auth::AuthModule::Params{
        .config = &config,
        .database_pool = nullptr,
        .router = router,
    });
    test::expect_true(module == nullptr, "auth module should reject missing database pool");
}

void test_auth_module_initializes_repository_service_and_routes() {
    const auto database_config = test::database::build_test_database_config();
    test::database::truncate_database_tables(database_config);
    auto database_pool = test::database::create_database_pool(database_config);
    auto router = std::make_shared<http::Router>();
    const test::TempDir dir("nebula-auth-module-test");

    auth::AuthConfig config;
    config.jwt_secret_path = dir.path() / "jwt.key";

    const auto module = auth::AuthModule::create(auth::AuthModule::Params{
        .config = &config,
        .database_pool = database_pool,
        .router = router,
    });
    test::expect_true(module != nullptr, "auth module create should succeed");
    test::expect_true(module->service() != nullptr, "auth module should expose initialized auth service");
    test::expect_true(router->has_route_exact(http::HttpMethod::Post, "/api/auth/register"),
                      "auth module should register register route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Post, "/api/auth/login"),
                      "auth module should register login route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Get, "/api/auth/me"),
                      "auth module should register me route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Put, "/api/auth/password"),
                      "auth module should register change password route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Get, "/api/auth/users"),
                      "auth module should register users list route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Get, "/api/auth/users/{user_id}"),
                      "auth module should register user detail route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Put, "/api/auth/users/{user_id}"),
                      "auth module should register user update route");
}

void test_auth_module_rejects_invalid_jwt_secret() {
    const auto database_config = test::database::build_test_database_config();
    test::database::truncate_database_tables(database_config);
    auto database_pool = test::database::create_database_pool(database_config);
    auto router = std::make_shared<http::Router>();
    const test::TempDir dir("nebula-auth-module-invalid-jwt-secret");

    const auto secret_path = dir.path() / "jwt.key";
    test::write_file(secret_path, "not-valid-base64!!!");
    test::set_owner_read_write_only(secret_path);

    auth::AuthConfig config;
    config.jwt_secret_path = secret_path;

    const auto module = auth::AuthModule::create(auth::AuthModule::Params{
        .config = &config,
        .database_pool = database_pool,
        .router = router,
    });
    test::expect_true(module == nullptr, "auth module should reject invalid jwt secret");
}

int run_module_tests() {
    test::database::require_database_test_env();

    std::vector<nebula::test::TestCase> tests = {
        {"auth module rejects missing database pool", test_auth_module_rejects_missing_database_pool},
        {"auth module initializes repository service and routes",
         test_auth_module_initializes_repository_service_and_routes},
        {"auth module rejects invalid jwt secret", test_auth_module_rejects_invalid_jwt_secret},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_module_tests);
}
