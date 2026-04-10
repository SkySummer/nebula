#include "nebula/server/http_server.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include "nebula/auth/auth_http.hpp"
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
using nebula::testsupport::database::load_postgres_pool_test_options;
using nebula::testsupport::database::validate_database_test_env;
using nebula::testsupport::integration::build_runtime;
using nebula::testsupport::integration::connect_localhost;
using nebula::testsupport::integration::read_until_close;
using nebula::testsupport::integration::send_all;
using nebula::testsupport::integration::wait_until_server_ready;
using ServerThreadGuard = nebula::testsupport::integration::ServerThreadGuard;

constexpr std::string_view kIntegrationJwtSecret = "integration_auth_secret_0123456789abcdef";

PostgresConnectionPoolOptions require_database_test_options() {
    const std::optional<PostgresConnectionPoolOptions> options = load_postgres_pool_test_options();
    if (!options.has_value()) {
        nebula::testsupport::fail("postgres test database env should be configured before running tests");
    }
    return options.value();
}

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

std::shared_ptr<nebula::http::Router> build_auth_router(const nebula::server::ServerConfig& config) {
    auto router = std::make_shared<nebula::http::Router>();
    expect_true(nebula::auth::register_auth_routes(config, router), "register auth routes should succeed");
    return router;
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
    const PostgresConnectionPoolOptions db_config = require_database_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-flow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto router = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, router);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        std::string(
            "POST /api/auth/register HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: ") +
        std::to_string(register_body.size()) + "\r\n\r\n" + register_body;
    const std::string register_response = send_single_request(server.listening_port(), register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");
    expect_contains(register_response, R"("code":"ok")", "register should return success code");
    expect_contains(register_response, R"("user_id":1)", "register should return numeric user_id");

    const std::string login_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string login_request =
        std::string(
            "POST /api/auth/login HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: ") +
        std::to_string(login_body.size()) + "\r\n\r\n" + login_body;
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

    const std::string me_request = std::string(
                                       "GET /api/auth/me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                       "Authorization: Bearer ") +
                                   token_value + "\r\n\r\n";
    const std::string me_response = send_single_request(server.listening_port(), me_request);
    expect_contains(me_response, "HTTP/1.1 200 OK", "me should return 200");
    expect_contains(me_response, R"("code":"ok")", "me should return success code");
    expect_contains(me_response, R"("user_id":1)", "me should return numeric user_id");
    expect_contains(me_response, R"("username":"Alice_1")", "me should return current user");
}

void test_auth_register_duplicate_returns_conflict() {
    const PostgresConnectionPoolOptions db_config = require_database_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-duplicate-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto router = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, router);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string request = std::string(
                                    "POST /api/auth/register HTTP/1.1\r\nHost: localhost\r\n"
                                    "Content-Type: application/json\r\nConnection: close\r\nContent-Length: ") +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string first_response = send_single_request(server.listening_port(), request);
    expect_contains(first_response, "HTTP/1.1 200 OK", "first register should return 200");

    const std::string duplicate_response = send_single_request(server.listening_port(), request);
    expect_contains(duplicate_response, "HTTP/1.1 409 Conflict", "duplicate register should return 409");
    expect_contains(duplicate_response, R"("code":"username_already_exists")",
                    "duplicate register should return conflict code");
}

void test_auth_login_invalid_password_returns_unauthorized() {
    const PostgresConnectionPoolOptions db_config = require_database_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-invalid-password-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto router = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, router);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        std::string(
            "POST /api/auth/register HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: ") +
        std::to_string(register_body.size()) + "\r\n\r\n" + register_body;
    const std::string register_response = send_single_request(server.listening_port(), register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");

    const std::string login_body = R"({"username":"Alice_1","password":"password_124"})";
    const std::string login_request =
        std::string(
            "POST /api/auth/login HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: ") +
        std::to_string(login_body.size()) + "\r\n\r\n" + login_body;
    const std::string login_response = send_single_request(server.listening_port(), login_request);
    expect_contains(login_response, "HTTP/1.1 401 Unauthorized", "invalid password login should return 401");
    expect_contains(login_response, R"("code":"invalid_credentials")",
                    "invalid password login should return invalid_credentials code");
}

void test_auth_me_missing_token_returns_unauthorized() {
    const PostgresConnectionPoolOptions db_config = require_database_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-me-missing-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    apply_database_config(config, db_config);
    auto router = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, router);

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

    const PostgresConnectionPoolOptions db_config = require_database_test_options();

    const nebula::testsupport::TempDir secret_dir("nebula-http-auth-expired-token-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.auth_access_token_ttl_s = 1;
    apply_database_config(config, db_config);
    auto router = build_auth_router(config);
    truncate_users_table(db_config);
    auto server = build_runtime(config, router);

    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string register_body = R"({"username":"Alice_1","password":"password_123"})";
    const std::string register_request =
        std::string(
            "POST /api/auth/register HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: ") +
        std::to_string(register_body.size()) + "\r\n\r\n" + register_body;
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

    const std::string me_request = std::string(
                                       "GET /api/auth/me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                       "Authorization: Bearer ") +
                                   token_value + "\r\n\r\n";
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
        return nebula::testsupport::database::kDatabaseTestSkipReturnCode;
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
