#include "nebula_tests/http.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/auth/bootstrap/module.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/time.hpp"
#include "nebula/common/security/crypto.hpp"
#include "nebula/database/config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"
#include "nebula_tests/integration.hpp"

namespace {

using namespace nebula;

struct AuthRouteRuntime {
    std::shared_ptr<nebula::database::ConnectionPool> database_pool;
    std::shared_ptr<nebula::http::Router> router;
    std::shared_ptr<nebula::auth::AuthService> auth_service;
};

AuthRouteRuntime build_auth_router(const nebula::app::AppConfig& config) {
    AuthRouteRuntime runtime;
    runtime.database_pool = test::database::create_database_pool(config.database);
    runtime.router = std::make_shared<nebula::http::Router>();
    const auto module = nebula::auth::AuthModule::create(nebula::auth::AuthModule::Params{
        .config = &config.auth,
        .database_pool = runtime.database_pool,
        .router = runtime.router,
    });
    test::expect_true(module != nullptr, "create auth module should succeed");
    runtime.auth_service = module->service();
    test::expect_true(runtime.auth_service != nullptr, "create auth service should succeed");
    return runtime;
}

std::optional<std::string> make_signed_access_token(std::span<const std::byte> secret, std::int64_t user_id,
                                                    std::int64_t iat_s, std::int64_t exp_s) {
    const std::string header = nebula::common::base64url_encode(R"({"alg":"HS256","typ":"JWT"})");
    const std::string payload = nebula::common::base64url_encode(
        std::format(R"({{"sub":"{}","ver":1,"iat":{},"exp":{}}})", user_id, iat_s, exp_s));
    const std::string signing_input = std::format("{}.{}", header, payload);
    const std::optional<std::string> signature = nebula::common::hmac_sha256(secret, signing_input);
    if (!signature.has_value()) {
        return std::nullopt;
    }
    return std::format("{}.{}", signing_input, nebula::common::base64url_encode(*signature));
}

std::string register_user_and_get_token(std::uint16_t port, std::string_view username, std::string_view password) {
    const std::string body =
        std::format(R"({{"username":"{}","password":"{}"}})", std::string(username), std::string(password));
    const std::string request = test::http::build_http_request("POST", "/api/auth/register", body, "application/json");
    const std::string response = test::http::send_single_request(port, request);
    test::expect_contains(response, "HTTP/1.1 200 OK", "register should return 200");
    const std::optional<std::string> token =
        test::http::extract_api_data_string_field(test::http::response_body(response), "access_token");
    if (!token.has_value() || token->empty()) {
        test::fail("register should return access token");
    }
    return *token;
}

void test_auth_register_login_me_flow_returns_role_and_status() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-flow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        test::http::build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = test::http::send_single_request(server.listening_port(), register_request);
    test::expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");
    test::expect_contains(register_response, R"("role":"owner")", "first registered user should be owner");
    test::expect_contains(register_response, R"("status":"active")", "register response should include active status");

    const std::string login_request =
        test::http::build_http_request("POST", "/api/auth/login", register_body, "application/json");
    const std::string login_response = test::http::send_single_request(server.listening_port(), login_request);
    test::expect_contains(login_response, "HTTP/1.1 200 OK", "login should return 200");
    test::expect_contains(login_response, R"("role":"owner")", "login response should include owner role");
    test::expect_contains(login_response, R"("status":"active")", "login response should include active status");

    const std::optional<std::string> token =
        test::http::extract_api_data_string_field(test::http::response_body(login_response), "access_token");
    if (!token.has_value() || token->empty()) {
        test::fail("login should return access token");
    }

    const std::string me_request = test::http::build_http_request("GET", "/api/auth/me", {}, {}, *token);
    const std::string me_response = test::http::send_single_request(server.listening_port(), me_request);
    test::expect_contains(me_response, "HTTP/1.1 200 OK", "me should return 200");
    test::expect_contains(me_response, R"("role":"owner")", "me response should include owner role");
    test::expect_contains(me_response, R"("status":"active")", "me response should include active status");
}

void test_auth_first_user_owner_second_user_user() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-owner-bootstrap-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string first_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/auth/register",
                                       R"({"username":"Alice_1","password":"password_123"})", "application/json"));
    test::expect_contains(first_response, R"("role":"owner")", "first registered user should be owner");

    const std::string second_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/auth/register",
                                       R"({"username":"Bob_1","password":"password_123"})", "application/json"));
    test::expect_contains(second_response, R"("role":"user")", "second registered user should default to user role");
}

void test_auth_change_password_success_and_invalid_current_password() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-change-password-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string invalid_change_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", "/api/auth/password",
                                       R"({"current_password":"wrong_password","new_password":"password_456"})",
                                       "application/json", token));
    test::expect_contains(invalid_change_response, "HTTP/1.1 401 Unauthorized",
                          "invalid current password should return 401");
    test::expect_contains(invalid_change_response, R"("code":"invalid_current_password")",
                          "invalid current password should return stable code");

    const std::string change_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", "/api/auth/password",
                                       R"({"current_password":"password_123","new_password":"password_456"})",
                                       "application/json", token));
    test::expect_contains(change_response, "HTTP/1.1 200 OK", "change password should return 200");

    const std::optional<std::string> rotated_token =
        test::http::extract_api_data_string_field(test::http::response_body(change_response), "access_token");
    if (!rotated_token.has_value() || rotated_token->empty()) {
        test::fail("change password should return new access token");
    }

    const std::string old_login_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/auth/login", R"({"username":"Alice_1","password":"password_123"})",
                                       "application/json"));
    test::expect_contains(old_login_response, "HTTP/1.1 401 Unauthorized", "old password should stop working");
    test::expect_contains(old_login_response, R"("code":"invalid_credentials")",
                          "old password should return invalid_credentials");

    const std::string new_login_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/auth/login", R"({"username":"Alice_1","password":"password_456"})",
                                       "application/json"));
    test::expect_contains(new_login_response, "HTTP/1.1 200 OK", "new password should work");

    const std::string old_me_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", "/api/auth/me", {}, {}, token));
    test::expect_contains(old_me_response, "HTTP/1.1 401 Unauthorized",
                          "old token should be invalid after password change");
    test::expect_contains(old_me_response, R"("code":"token_invalid")", "old token should return token_invalid");

    const std::string new_me_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", "/api/auth/me", {}, {}, *rotated_token));
    test::expect_contains(new_me_response, "HTTP/1.1 200 OK", "rotated token should authenticate");
}

void test_auth_owner_can_promote_and_disable_user() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-owner-manage-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string owner_token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");
    const std::string user_token = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");

    const std::string list_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", "/api/auth/users?limit=1&offset=0", {}, {}, owner_token));
    test::expect_contains(list_response, "HTTP/1.1 200 OK", "owner should be able to list users");
    test::expect_contains(list_response, R"("has_more":true)", "user list should report has_more");

    const std::string promote_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("PUT", "/api/auth/users/2", R"({"role":"admin"})",
                                                                "application/json", owner_token));
    test::expect_contains(promote_response, "HTTP/1.1 200 OK", "owner should be able to promote user");
    test::expect_contains(promote_response, R"("role":"admin")", "promote response should return admin role");

    const std::string detail_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", "/api/auth/users/2", {}, {}, owner_token));
    test::expect_contains(detail_response, "HTTP/1.1 200 OK", "owner should be able to get user detail");
    test::expect_contains(detail_response, R"("role":"admin")", "detail response should reflect promoted role");

    const std::string disable_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("PUT", "/api/auth/users/2", R"({"status":"disabled"})",
                                                                "application/json", owner_token));
    test::expect_contains(disable_response, "HTTP/1.1 200 OK", "owner should be able to disable user");
    test::expect_contains(disable_response, R"("status":"disabled")", "disable response should return disabled status");

    const std::string disabled_me_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", "/api/auth/me", {}, {}, user_token));
    test::expect_contains(disabled_me_response, "HTTP/1.1 403 Forbidden",
                          "disabled user existing token should be rejected");
    test::expect_contains(disabled_me_response, R"("code":"user_disabled")",
                          "disabled user existing token should return stable code");
}

void test_auth_admin_can_manage_user_but_not_owner() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-admin-manage-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string owner_token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");
    const std::string admin_token = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");
    [[maybe_unused]] const std::string user_token =
        register_user_and_get_token(server.listening_port(), "Charlie_1", "password_123");

    const std::string promote_admin_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("PUT", "/api/auth/users/2", R"({"role":"admin"})",
                                                                "application/json", owner_token));
    test::expect_contains(promote_admin_response, "HTTP/1.1 200 OK", "owner should be able to promote admin");

    const std::string admin_update_user_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", "/api/auth/users/3", R"({"role":"admin","status":"disabled"})",
                                       "application/json", admin_token));
    test::expect_contains(admin_update_user_response, "HTTP/1.1 200 OK", "admin should be able to manage user");
    test::expect_contains(admin_update_user_response, R"("role":"admin")", "admin managed user should be promoted");
    test::expect_contains(admin_update_user_response, R"("status":"disabled")",
                          "admin managed user should be disabled");

    const std::string admin_update_owner_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("PUT", "/api/auth/users/1", R"({"status":"disabled"})",
                                                                "application/json", admin_token));
    test::expect_contains(admin_update_owner_response, "HTTP/1.1 403 Forbidden",
                          "admin should not be able to manage owner");
    test::expect_contains(admin_update_owner_response, R"("code":"forbidden")",
                          "admin owner management should return forbidden");
}

void test_auth_me_missing_token_returns_unauthorized() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-me-missing-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string response = test::http::send_single_request(
        server.listening_port(), "GET /api/auth/me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    test::expect_contains(response, "HTTP/1.1 401 Unauthorized", "me without token should return 401");
    test::expect_contains(response, R"("code":"token_missing")", "me without token should return token_missing code");
}

void test_auth_me_expired_token_returns_unauthorized() {
    const nebula::test::TempDir secret_dir("nebula-http-auth-expired-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.auth.access_token_ttl_s = 60;
    config.database = test::database::build_test_database_config();
    auto auth_runtime = build_auth_router(config);
    test::database::truncate_database_tables(config.database);
    auto server = test::integration::build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::int64_t now_s = nebula::common::now_epoch_s();
    const std::optional<std::string> token = make_signed_access_token(
        test::to_byte_vector(test::integration::kIntegrationJwtSecret), 1, now_s - 120, now_s - 60);
    if (!token.has_value() || token->empty()) {
        test::fail("expired access token should be generated");
    }

    const std::string me_request = test::http::build_http_request("GET", "/api/auth/me", {}, {}, *token);
    const std::string me_response = test::http::send_single_request(server.listening_port(), me_request);
    test::expect_contains(me_response, "HTTP/1.1 401 Unauthorized", "me with expired token should return 401");
    test::expect_contains(me_response, R"("code":"token_expired")", "expired token should return token_expired code");
}

int run_http_server_auth_db_integration_tests() {
    nebula::common::Logger::instance().set_level(nebula::common::LogLevel::Warning);
    test::database::require_database_test_env();

    const std::vector<nebula::test::TestCase> tests = {
        {"auth register login me flow returns role and status",
         test_auth_register_login_me_flow_returns_role_and_status},
        {"auth first user owner second user user", test_auth_first_user_owner_second_user_user},
        {"auth change password success and invalid current password",
         test_auth_change_password_success_and_invalid_current_password},
        {"auth owner can promote and disable user", test_auth_owner_can_promote_and_disable_user},
        {"auth admin can manage user but not owner", test_auth_admin_can_manage_user_but_not_owner},
        {"auth me missing token returns unauthorized", test_auth_me_missing_token_returns_unauthorized},
        {"auth me expired token returns unauthorized", test_auth_me_expired_token_returns_unauthorized},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_http_server_auth_db_integration_tests);
}
