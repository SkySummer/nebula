#include "nebula/server/http_server.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include "nebula/auth/auth_http.hpp"
#include "nebula/auth/auth_service.hpp"
#include "nebula/common/database_utils.hpp"
#include "nebula/common/json.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/http/router.hpp"
#include "nebula_tests/test_support.hpp"
#include "nebula_tests/test_support_database.hpp"
#include "nebula_tests/test_support_integration.hpp"

namespace {

using nebula::common::PostgresConnectionPoolOptions;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_true;
using nebula::testsupport::write_jwt_secret_file;
using nebula::testsupport::database::require_postgres_pool_test_options;
using nebula::testsupport::database::validate_database_test_env;
using nebula::testsupport::integration::build_runtime;
using nebula::testsupport::integration::connect_localhost;
using nebula::testsupport::integration::read_until_close;
using nebula::testsupport::integration::send_all;
using nebula::testsupport::integration::ServerThreadGuard;
using nebula::testsupport::integration::wait_until_server_ready;

constexpr std::string_view kIntegrationJwtSecret = "integration_auth_secret_0123456789abcdef";

void apply_database_config(nebula::server::ServerConfig& config, const PostgresConnectionPoolOptions& db_config) {
    config.database_host = db_config.host;
    config.database_port = db_config.port;
    config.database_name = db_config.database;
    config.database_user = db_config.user;
    ::setenv("NEBULA_TEST_DATABASE_PASSWORD_RUNTIME", db_config.password.c_str(), 1);
    config.database_password_env = "NEBULA_TEST_DATABASE_PASSWORD_RUNTIME";
}

void truncate_users_table(const PostgresConnectionPoolOptions& config) {
    pqxx::connection connection(nebula::common::build_connection_info(config));
    pqxx::work tx(connection);
    tx.exec("TRUNCATE TABLE users RESTART IDENTITY");
    tx.exec("ALTER SEQUENCE users_user_id_seq RESTART WITH 1");
    tx.commit();
}

struct AuthRouteRuntime {
    std::shared_ptr<nebula::http::Router> router;
    std::shared_ptr<nebula::auth::AuthService> auth_service;
};

void ensure_database_pool_initialized(const nebula::server::ServerConfig& config) {
    const std::optional<std::string> password = nebula::common::resolve_database_password(config.database_password_env);
    expect_true(password.has_value(), "database password should be available");
    if (!password.has_value()) {
        return;
    }

    const nebula::common::PostgresConnectionPool::InitializeStatus status =
        nebula::common::PostgresConnectionPool::instance().initialize(nebula::common::PostgresConnectionPoolOptions{
            .host = config.database_host,
            .port = config.database_port,
            .database = config.database_name,
            .user = config.database_user,
            .password = *password,
            .max_connections = config.database_max_connections,
            .connect_timeout_ms = config.database_connect_timeout_ms,
            .acquire_timeout_ms = config.database_acquire_timeout_ms,
        });
    expect_true(status == nebula::common::PostgresConnectionPool::InitializeStatus::Initialized ||
                    status == nebula::common::PostgresConnectionPool::InitializeStatus::AlreadyInitialized,
                "database pool initialization should succeed");
}

AuthRouteRuntime build_auth_router(const nebula::server::ServerConfig& config) {
    AuthRouteRuntime runtime;
    runtime.router = std::make_shared<nebula::http::Router>();
    ensure_database_pool_initialized(config);
    runtime.auth_service = nebula::auth::initialize_auth_service(config);
    expect_true(runtime.auth_service != nullptr, "initialize auth service should succeed");
    if (runtime.auth_service != nullptr) {
        expect_true(nebula::auth::register_auth_routes(runtime.auth_service, runtime.router),
                    "register auth routes should succeed");
    }
    return runtime;
}

std::string send_single_request(std::uint16_t port, std::string_view request) {
    const int fd = connect_localhost(port);
    expect_true(fd >= 0, "connect should succeed");
    expect_true(send_all(fd, request), "send request should succeed");
    const std::string response = read_until_close(fd);
    ::close(fd);
    return response;
}

std::string response_body(std::string_view response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return {};
    }
    return std::string(response.substr(header_end + 4U));
}

std::string build_http_request(std::string_view method, std::string_view path, std::string_view body = {},
                               std::string_view content_type = {}, std::string_view access_token = {}) {
    const std::string content_type_header =
        content_type.empty() ? std::string() : std::format("Content-Type: {}\r\n", content_type);
    const std::string auth_header =
        access_token.empty() ? std::string() : std::format("Authorization: Bearer {}\r\n", access_token);
    const bool include_content_length = !body.empty() || method == "POST" || method == "PUT" || method == "DELETE";
    const std::string content_length_header =
        include_content_length ? std::format("Content-Length: {}\r\n", body.size()) : std::string();
    return std::format("{} {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}{}{}\r\n{}", method, path,
                       content_type_header, auth_header, content_length_header, body);
}

std::optional<std::string> extract_api_data_string_field(std::string_view json, std::string_view key) {
    const nebula::common::JsonParseResult parsed = nebula::common::parse_json(json);
    if (!parsed.ok) {
        return std::nullopt;
    }

    const nebula::common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }

    const auto data_it = object->find("data");
    if (data_it == object->end()) {
        return std::nullopt;
    }

    const nebula::common::JsonObject* data = data_it->second.get_if_object();
    if (data == nullptr) {
        return std::nullopt;
    }

    const auto value_it = data->find(std::string(key));
    if (value_it == data->end()) {
        return std::nullopt;
    }

    const std::string* value = value_it->second.get_if_string();
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

void test_auth_register_login_me_flow() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-flow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto auth_runtime = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = send_single_request(server.listening_port(), register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");
    expect_contains(register_response, R"("code":"ok")", "register should return success code");
    expect_contains(register_response, R"("user_id":1)", "register should return numeric user_id");

    const std::string login_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string login_request = build_http_request("POST", "/api/auth/login", login_body, "application/json");
    const std::string login_response = send_single_request(server.listening_port(), login_request);
    expect_contains(login_response, "HTTP/1.1 200 OK", "login should return 200");
    expect_contains(login_response, R"("code":"ok")", "login should return success code");

    const std::optional<std::string> token =
        extract_api_data_string_field(response_body(login_response), "access_token");
    expect_true(token.has_value() && !token->empty(), "login should return access token");
    if (!token.has_value() || token->empty()) {
        nebula::testsupport::fail("login should return access token");
    }
    const std::string& token_value = *token;

    const std::string me_request = build_http_request("GET", "/api/auth/me", {}, {}, token_value);
    const std::string me_response = send_single_request(server.listening_port(), me_request);
    expect_contains(me_response, "HTTP/1.1 200 OK", "me should return 200");
    expect_contains(me_response, R"("code":"ok")", "me should return success code");
    expect_contains(me_response, R"("user_id":1)", "me should return numeric user_id");
    expect_contains(me_response, R"("username":"Alice_1")", "me should return current user");
}

void test_auth_register_duplicate_returns_conflict() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-duplicate-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto auth_runtime = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string request = build_http_request("POST", "/api/auth/register", body, "application/json");
    const std::string first_response = send_single_request(server.listening_port(), request);
    expect_contains(first_response, "HTTP/1.1 200 OK", "first register should return 200");

    const std::string duplicate_response = send_single_request(server.listening_port(), request);
    expect_contains(duplicate_response, "HTTP/1.1 409 Conflict", "duplicate register should return 409");
    expect_contains(duplicate_response, R"("code":"username_already_exists")",
                    "duplicate register should return conflict code");
}

void test_auth_login_invalid_password_returns_unauthorized() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-invalid-password-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto auth_runtime = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = send_single_request(server.listening_port(), register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");

    const std::string login_body = R"({"username":"Alice_1","password":"password_124"})";
    const std::string login_request = build_http_request("POST", "/api/auth/login", login_body, "application/json");
    const std::string login_response = send_single_request(server.listening_port(), login_request);
    expect_contains(login_response, "HTTP/1.1 401 Unauthorized", "invalid password login should return 401");
    expect_contains(login_response, R"("code":"invalid_credentials")",
                    "invalid password login should return invalid_credentials code");
}

void test_auth_me_missing_token_returns_unauthorized() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-me-missing-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto auth_runtime = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string response = send_single_request(
        server.listening_port(), "GET /api/auth/me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    expect_contains(response, "HTTP/1.1 401 Unauthorized", "me without token should return 401");
    expect_contains(response, R"("code":"token_missing")", "me without token should return token_missing code");
}

void test_auth_me_expired_token_returns_unauthorized() {
    using namespace std::chrono_literals;

    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-expired-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.auth_access_token_ttl_s = 1;
    apply_database_config(config, db_config);
    auto auth_runtime = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, auth_runtime.router, auth_runtime.auth_service);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = send_single_request(server.listening_port(), register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");

    const std::optional<std::string> token =
        extract_api_data_string_field(response_body(register_response), "access_token");
    expect_true(token.has_value() && !token->empty(), "register should return access token");
    if (!token.has_value() || token->empty()) {
        nebula::testsupport::fail("register should return access token");
    }
    const std::string& token_value = *token;

    std::this_thread::sleep_for(2s);

    const std::string me_request = build_http_request("GET", "/api/auth/me", {}, {}, token_value);
    const std::string me_response = send_single_request(server.listening_port(), me_request);
    expect_contains(me_response, "HTTP/1.1 401 Unauthorized", "me with expired token should return 401");
    expect_contains(me_response, R"("code":"token_expired")", "expired token should return token_expired code");
}

int run_http_server_auth_db_integration_tests() {
    const nebula::testsupport::TempDir log_dir("nebula-http-server-auth-db-integration-log");
    nebula::common::Logger::instance().initialize(nebula::common::LogLevel::Warning, log_dir.path(), false);

    const std::optional<std::string> env_error = validate_database_test_env();
    if (env_error.has_value()) {
        std::cerr << "[SKIP] http server auth db integration precheck skipped: error=" << *env_error << '\n';
        return nebula::testsupport::kTestSkipReturnCode;
    }

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"auth register login me flow", test_auth_register_login_me_flow},
        {"auth register duplicate returns conflict", test_auth_register_duplicate_returns_conflict},
        {"auth login invalid password returns unauthorized", test_auth_login_invalid_password_returns_unauthorized},
        {"auth me missing token returns unauthorized", test_auth_me_missing_token_returns_unauthorized},
        {"auth me expired token returns unauthorized", test_auth_me_expired_token_returns_unauthorized},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_http_server_auth_db_integration_tests);
}
