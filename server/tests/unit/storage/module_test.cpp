#include "nebula/storage/bootstrap/module.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include "nebula/http/config/http_limits_config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/storage/bootstrap/config.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"

namespace {

using namespace nebula;

void test_storage_module_rejects_missing_database_pool() {
    auto router = std::make_shared<http::Router>();
    const storage::StorageConfig config{};
    const http::HttpLimitsConfig limits{};
    const auto module = storage::StorageModule::create(storage::StorageModule::Params{
        .config = &config,
        .limits = &limits,
        .database_pool = nullptr,
        .router = router,
    });
    test::expect_true(module == nullptr, "storage module should reject missing database pool");
}

void test_storage_module_initializes_repository_routes_and_storage_dirs() {
    const auto database_config = test::database::build_test_database_config();
    test::database::truncate_database_tables(database_config);
    auto database_pool = test::database::create_database_pool(database_config);
    auto router = std::make_shared<http::Router>();
    const test::TempDir dir("nebula-storage-module-test");

    storage::StorageConfig config;
    config.root_dir = dir.path() / "files";

    const http::HttpLimitsConfig limits{};
    const auto module = storage::StorageModule::create(storage::StorageModule::Params{
        .config = &config,
        .limits = &limits,
        .database_pool = database_pool,
        .router = router,
    });
    test::expect_true(module != nullptr, "storage module create should succeed");
    test::expect_true(std::filesystem::is_directory(config.root_dir / "temp"),
                      "storage module should create temp storage directory");
    test::expect_true(std::filesystem::is_directory(config.root_dir / "objects"),
                      "storage module should create object storage directory");
    test::expect_true(router->has_route_exact(http::HttpMethod::Post, "/api/storage/uploads/init"),
                      "storage module should register upload init route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Get, "/api/storage/recent"),
                      "storage module should register recent route");
    test::expect_true(router->has_route_exact(http::HttpMethod::Get, "/api/storage/usage"),
                      "storage module should register usage route");
}

int run_module_tests() {
    test::database::require_database_test_env();

    std::vector<nebula::test::TestCase> tests = {
        {"storage module rejects missing database pool", test_storage_module_rejects_missing_database_pool},
        {"storage module initializes repository routes and storage dirs",
         test_storage_module_initializes_repository_routes_and_storage_dirs},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_module_tests);
}
