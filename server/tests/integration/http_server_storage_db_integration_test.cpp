#include "nebula/server/http_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
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
#include "nebula/common/base64.hpp"
#include "nebula/common/database_utils.hpp"
#include "nebula/common/json.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/time_utils.hpp"
#include "nebula/http/router.hpp"
#include "nebula/storage/storage_http.hpp"
#include "nebula/storage/storage_repository.hpp"
#include "nebula_tests/test_support.hpp"
#include "nebula_tests/test_support_database.hpp"
#include "nebula_tests/test_support_integration.hpp"

namespace {

using nebula::common::PostgresConnectionPoolOptions;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_not_contains;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_jwt_secret_file;
using nebula::testsupport::database::require_postgres_pool_test_options;
using nebula::testsupport::integration::build_runtime;
using nebula::testsupport::integration::connect_localhost;
using nebula::testsupport::integration::read_until_close;
using nebula::testsupport::integration::send_all;
using nebula::testsupport::integration::ServerThreadGuard;
using nebula::testsupport::integration::wait_until_server_ready;

constexpr std::string_view kIntegrationJwtSecret = "integration_storage_secret_0123456789abcdef";
void apply_database_config(nebula::server::ServerConfig& config, const PostgresConnectionPoolOptions& db_config) {
    config.database_host = db_config.host;
    config.database_port = db_config.port;
    config.database_name = db_config.database;
    config.database_user = db_config.user;
    ::setenv("NEBULA_TEST_DATABASE_PASSWORD_RUNTIME", db_config.password.c_str(), 1);
    config.database_password_env = "NEBULA_TEST_DATABASE_PASSWORD_RUNTIME";
}

void truncate_storage_tables(const PostgresConnectionPoolOptions& config) {
    pqxx::connection connection(nebula::common::build_connection_info(config));
    pqxx::work tx(connection);
    tx.exec("TRUNCATE TABLE storage_nodes, storage_upload_sessions, storage_objects");
    tx.exec("TRUNCATE TABLE users RESTART IDENTITY");
    tx.exec("ALTER SEQUENCE users_user_id_seq RESTART WITH 1");
    tx.commit();
}

struct StorageRouteRuntime {
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

StorageRouteRuntime build_storage_router(const nebula::server::ServerConfig& config) {
    StorageRouteRuntime runtime;
    runtime.router = std::make_shared<nebula::http::Router>();
    ensure_database_pool_initialized(config);
    runtime.auth_service = nebula::auth::initialize_auth_service(config);
    expect_true(runtime.auth_service != nullptr, "initialize auth service should succeed");
    if (runtime.auth_service != nullptr) {
        expect_true(nebula::auth::register_auth_routes(runtime.auth_service, runtime.router),
                    "register auth routes should succeed");
        expect_true(nebula::storage::register_storage_routes(config, runtime.router),
                    "register storage routes should succeed");
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

std::string build_http_request(std::string_view method, std::string_view path, std::string_view body,
                               std::string_view content_type = "application/json", std::string_view access_token = "") {
    const std::string auth_header =
        access_token.empty() ? std::string() : std::format("Authorization: Bearer {}\r\n", access_token);
    const bool include_content_headers = !body.empty() || method == "POST" || method == "PUT" || method == "DELETE";
    const std::string content_headers =
        include_content_headers ? std::format("Content-Type: {}\r\nContent-Length: {}\r\n", content_type, body.size())
                                : std::string();
    return std::format("{} {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}{}\r\n{}", method, path,
                       auth_header, content_headers, body);
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
    const auto it = data->find(std::string(key));
    if (it == data->end()) {
        return std::nullopt;
    }
    const std::string* value = it->second.get_if_string();
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

std::optional<std::int64_t> fetch_object_ref_count(const PostgresConnectionPoolOptions& config,
                                                   std::string_view sha256) {
    pqxx::connection connection(nebula::common::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    const pqxx::result rows =
        tx.exec_params("SELECT ref_count FROM storage_objects WHERE sha256 = $1 LIMIT 1", std::string(sha256));
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front()[0].as<std::int64_t>(0);
}

std::int64_t fetch_object_count(const PostgresConnectionPoolOptions& config) {
    pqxx::connection connection(nebula::common::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    return tx.exec1("SELECT COUNT(*) FROM storage_objects")[0].as<std::int64_t>(0);
}

template <typename Write>
void expect_storage_node_write_rejected(const PostgresConnectionPoolOptions& config, Write write,
                                        std::string_view message) {
    try {
        pqxx::connection connection(nebula::common::build_connection_info(config));
        pqxx::work tx(connection);
        write(tx);
        tx.commit();
        nebula::testsupport::fail(message);
    } catch (const pqxx::sql_error& e) {
        expect_equal(std::string(e.sqlstate()), std::string("23514"), message);
    }
}

void write_test_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    expect_true(stream.is_open(), "test file should open for writing");
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    expect_true(stream.good(), "test file write should succeed");
}

void make_file_stale(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stale_time = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(120);
    std::filesystem::last_write_time(path, stale_time, ec);
    expect_true(!ec, "set stale file mtime should succeed");
}

std::string register_user_and_get_token(std::uint16_t port, std::string_view username, std::string_view password) {
    const std::string register_body =
        std::format(R"({{"username":"{}","password":"{}"}})", std::string(username), std::string(password));
    const std::string register_request =
        build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = send_single_request(port, register_request);
    expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");
    const std::optional<std::string> token =
        extract_api_data_string_field(response_body(register_response), "access_token");
    expect_true(token.has_value() && !token->empty(), "register should return access token");
    if (!token.has_value() || token->empty()) {
        nebula::testsupport::fail("register should return access token");
    }
    return *token;
}

std::string init_and_upload_single_chunk(std::uint16_t port, std::string_view path, std::string_view content,
                                         std::string_view access_token) {
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_request =
        build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", access_token);
    const std::string init_response = send_single_request(port, init_request);
    expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");

    const std::optional<std::string> upload_id =
        extract_api_data_string_field(response_body(init_response), "upload_id");
    expect_true(upload_id.has_value(), "upload init should return upload_id");
    if (!upload_id.has_value()) {
        nebula::testsupport::fail("upload init should return upload_id");
    }

    const std::string chunk_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk_request =
        build_http_request("PUT", chunk_path, content, "application/octet-stream", access_token);
    const std::string chunk_response = send_single_request(port, chunk_request);
    expect_contains(chunk_response, "HTTP/1.1 200 OK", "upload chunk should return 200");

    return *upload_id;
}

std::string complete_upload(std::uint16_t port, std::string_view upload_id, std::string_view access_token) {
    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", upload_id);
    const std::string complete_request =
        build_http_request("POST", complete_path, "", "application/json", access_token);
    return send_single_request(port, complete_request);
}

void ensure_directory_exists(std::uint16_t port, std::string_view path, std::string_view access_token) {
    if (path == "/") {
        return;
    }

    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string response =
        send_single_request(port, build_http_request("PUT", std::format("/api/storage/directories/{}", path_b64), "",
                                                     "application/json", access_token));
    const bool created = response.find("HTTP/1.1 200 OK") != std::string::npos;
    const bool already_exists = response.find("HTTP/1.1 409 Conflict") != std::string::npos &&
                                response.find(R"("code":"directory_already_exists")") != std::string::npos;
    expect_true(created || already_exists, "directory setup should create or find existing directory");
}

void ensure_parent_directories(std::uint16_t port, std::string_view file_path, std::string_view access_token) {
    for (std::size_t slash_pos = file_path.find('/', 1); slash_pos != std::string_view::npos;
         slash_pos = file_path.find('/', slash_pos + 1U)) {
        ensure_directory_exists(port, file_path.substr(0, slash_pos), access_token);
    }
}

void expect_invalid_storage_path_response(std::uint16_t port, std::string_view request, std::string_view message) {
    const std::string response = send_single_request(port, request);
    expect_contains(response, "HTTP/1.1 400 Bad Request", message);
    expect_contains(response, R"("code":"invalid_path")", message);
}

std::string upload_file_single_chunk(std::uint16_t port, std::string_view path, std::string_view content,
                                     std::string_view access_token) {
    ensure_parent_directories(port, path, access_token);
    const std::string upload_id = init_and_upload_single_chunk(port, path, content, access_token);
    const std::string complete_response = complete_upload(port, upload_id, access_token);
    expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should return 200");

    return response_body(complete_response);
}

void test_storage_node_schema_constraints() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();

    nebula::server::ServerConfig config;
    apply_database_config(config, db_config);
    ensure_database_pool_initialized(config);
    expect_true(nebula::storage::check_storage_schema_ready(),
                "storage schema readiness should require node type constraints");

    truncate_storage_tables(db_config);

    const std::string sha256(64U, 'b');
    const std::string object_rel_path =
        std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
    {
        pqxx::connection connection(nebula::common::build_connection_info(db_config));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, 5, $2, 0, 1000, 1000)",
            sha256, object_rel_path);
        tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, 5, 1000)",
                       std::string("/users/1/schema-valid-file.txt"), sha256);
        tx.exec_params(
            "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'directory', "
            "NULL, NULL, 1000)",
            std::string("/users/1/schema-valid-directory"));
        tx.commit();
    }

    expect_storage_node_write_rejected(
        db_config,
        [](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'symlink', "
                "NULL, NULL, 1000)",
                std::string("/users/1/schema-invalid-type"));
        },
        "unknown node_type should be rejected by schema");
    expect_storage_node_write_rejected(
        db_config,
        [](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'file', NULL, "
                "5, 1000)",
                std::string("/users/1/schema-file-missing-sha.txt"));
        },
        "file node should reject missing sha");
    expect_storage_node_write_rejected(
        db_config,
        [&sha256](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'file', $2, "
                "NULL, 1000)",
                std::string("/users/1/schema-file-missing-size.txt"), sha256);
        },
        "file node should reject missing size");
    expect_storage_node_write_rejected(
        db_config,
        [&sha256](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'directory', "
                "$2, 5, 1000)",
                std::string("/users/1/schema-directory-with-file-fields"), sha256);
        },
        "directory node should reject file fields");
}

void test_storage_requires_access_token() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-auth-required-files");
    const TempDir secret_dir("nebula-storage-auth-required-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string path_b64 = nebula::common::base64url_encode("/docs/unauthorized.txt");
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_request = build_http_request("POST", "/api/storage/uploads/init", init_body);
    const std::string init_response = send_single_request(server.listening_port(), init_request);
    expect_contains(init_response, "HTTP/1.1 401 Unauthorized", "storage request without token should return 401");
    expect_contains(init_response, R"("code":"token_missing")",
                    "storage request without token should return token_missing");
}

void test_storage_rejects_non_canonical_user_paths() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-invalid-path-files");
    const TempDir secret_dir("nebula-storage-invalid-path-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::array<std::string_view, 5> invalid_paths = {
        "a/../b", "/a/../b", "/a//b", "/a/./b", "/a/b/",
    };
    for (const std::string_view invalid_path : invalid_paths) {
        const std::string path_b64 = nebula::common::base64url_encode(invalid_path);
        const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
        expect_invalid_storage_path_response(
            server.listening_port(),
            build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token),
            "upload init should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            build_http_request("PUT", std::format("/api/storage/directories/{}", path_b64), "", "application/json",
                               token),
            "create directory should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            build_http_request("GET", std::format("/api/storage/files/{}", path_b64), "", "application/json", token),
            "file download should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            build_http_request("GET", std::format("/api/storage/tree/{}", path_b64), "", "application/json", token),
            "tree list should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            build_http_request("DELETE", std::format("/api/storage/nodes/{}", path_b64), "", "application/json", token),
            "delete node should reject non-canonical storage path");
    }
}

void test_storage_upload_complete_download_flow() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-files");
    const TempDir secret_dir("nebula-storage-flow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path = "/docs/hello.txt";
    ensure_directory_exists(server.listening_port(), "/docs", token);
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":2}})", path_b64);
    const std::string init_request =
        build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token);
    const std::string init_response = send_single_request(server.listening_port(), init_request);
    expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");
    const std::optional<std::string> upload_id =
        extract_api_data_string_field(response_body(init_response), "upload_id");
    expect_true(upload_id.has_value(), "upload init should return upload_id");
    if (!upload_id.has_value()) {
        nebula::testsupport::fail("upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_request =
        build_http_request("PUT", chunk0_path, "hello ", "application/octet-stream", token);
    const std::string chunk0_response = send_single_request(server.listening_port(), chunk0_request);
    expect_contains(chunk0_response, "HTTP/1.1 200 OK", "chunk 0 should return 200");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_request =
        build_http_request("PUT", chunk1_path, "nebula", "application/octet-stream", token);
    const std::string chunk1_response = send_single_request(server.listening_port(), chunk1_request);
    expect_contains(chunk1_response, "HTTP/1.1 200 OK", "chunk 1 should return 200");

    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", *upload_id);
    const std::string complete_request = build_http_request("POST", complete_path, "", "application/json", token);
    const std::string complete_response = send_single_request(server.listening_port(), complete_request);
    expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should return 200");

    const std::string complete_again_response = send_single_request(server.listening_port(), complete_request);
    expect_contains(complete_again_response, "HTTP/1.1 404 Not Found", "completed upload should not complete again");
    expect_contains(complete_again_response, R"("code":"upload_not_found")",
                    "completed upload should report upload_not_found on repeat complete");
    expect_contains(complete_again_response, R"("message":"upload not found")",
                    "completed upload should report user-facing missing upload message");

    const std::string download_path = std::format("/api/storage/files/{}", path_b64);
    const std::string download_request = build_http_request("GET", download_path, "", "application/json", token);
    const std::string download_response = send_single_request(server.listening_port(), download_request);
    expect_contains(download_response, "HTTP/1.1 200 OK", "download should return 200");
    expect_equal(response_body(download_response), std::string("hello nebula"), "download body should match upload");
}

void test_storage_upload_rejects_chunk_after_expected_count() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-upload-bound-files");
    const TempDir secret_dir("nebula-storage-upload-bound-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path = "/docs/chunk-bound.txt";
    ensure_directory_exists(server.listening_port(), "/docs", token);
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_response = send_single_request(
        server.listening_port(),
        build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token));
    expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");
    const std::optional<std::string> upload_id =
        extract_api_data_string_field(response_body(init_response), "upload_id");
    expect_true(upload_id.has_value(), "upload init should return upload_id");
    if (!upload_id.has_value()) {
        nebula::testsupport::fail("upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_response = send_single_request(
        server.listening_port(), build_http_request("PUT", chunk0_path, "first", "application/octet-stream", token));
    expect_contains(chunk0_response, "HTTP/1.1 200 OK", "first chunk should return 200");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_response = send_single_request(
        server.listening_port(),
        build_http_request("PUT", chunk1_path, "-should-be-rejected", "application/octet-stream", token));
    expect_contains(chunk1_response, "HTTP/1.1 409 Conflict", "chunk beyond total_chunks should return 409");
    expect_contains(chunk1_response, R"("code":"invalid_chunk_index")",
                    "chunk beyond total_chunks should return invalid_chunk_index");

    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", *upload_id);
    const std::string complete_response = send_single_request(
        server.listening_port(), build_http_request("POST", complete_path, "", "application/json", token));
    expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should still return 200");

    const std::string download_path = std::format("/api/storage/files/{}", path_b64);
    const std::string download_response = send_single_request(
        server.listening_port(), build_http_request("GET", download_path, "", "application/json", token));
    expect_contains(download_response, "HTTP/1.1 200 OK", "download should return 200");
    expect_equal(response_body(download_response), std::string("first"), "extra rejected chunk must not be persisted");
}

void test_storage_upload_chunk_failure_restores_db_size() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-upload-restore-files");

    nebula::server::ServerConfig config;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);
    ensure_database_pool_initialized(config);

    const std::filesystem::path temp_abs_path = config.storage_root_dir / "temp/retry.part";
    std::filesystem::create_directories(temp_abs_path.parent_path());
    {
        std::ofstream stream(temp_abs_path, std::ios::binary | std::ios::trunc);
        expect_true(stream.is_open(), "temp upload file should open");
    }

    const std::string upload_id = "retry-after-partial-write";
    const nebula::storage::UploadSessionRecord session{
        .upload_id = upload_id,
        .path = "/users/1/retry.txt",
        .temp_rel_path = "temp/retry.part",
        .total_chunks = 1,
        .next_chunk_index = 0,
        .temp_size_bytes = 0,
    };
    const nebula::storage::UploadSessionCreateResult create_result =
        nebula::storage::create_upload_session(session, 1, 1000);
    expect_true(create_result.status == nebula::storage::UploadSessionCreateStatus::Created,
                "upload session should be created");

    const nebula::storage::UploadChunkAppendResult failed_append = nebula::storage::append_upload_chunk(
        upload_id, 1, 0, 1001, config.storage_root_dir,
        [](const std::filesystem::path& path, std::int64_t) {
            std::ofstream stream(path, std::ios::binary | std::ios::app);
            stream << "partial";
            stream.flush();
            return nebula::storage::UploadChunkFileAppendResult{
                .status = nebula::storage::UploadChunkFileAppendStatus::Failed, .bytes_written = 7};
        },
        [](const std::filesystem::path& path, std::int64_t committed_size_bytes) {
            std::error_code ec;
            std::filesystem::resize_file(path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
            return !ec;
        });
    expect_true(failed_append.status == nebula::storage::UploadChunkAppendStatus::InternalError,
                "partial append failure should return internal error");
    expect_equal(nebula::testsupport::read_all(temp_abs_path), std::string(),
                 "failed chunk append should restore file to db committed size");

    const nebula::storage::UploadChunkAppendResult retry_append = nebula::storage::append_upload_chunk(
        upload_id, 1, 0, 1002, config.storage_root_dir,
        [](const std::filesystem::path& path, std::int64_t) {
            std::ofstream stream(path, std::ios::binary | std::ios::app);
            stream << "complete";
            stream.flush();
            return nebula::storage::UploadChunkFileAppendResult{
                .status = stream.good() ? nebula::storage::UploadChunkFileAppendStatus::Appended
                                        : nebula::storage::UploadChunkFileAppendStatus::Failed,
                .bytes_written = 8};
        },
        [](const std::filesystem::path& path, std::int64_t committed_size_bytes) {
            std::error_code ec;
            std::filesystem::resize_file(path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
            return !ec;
        });
    expect_true(retry_append.status == nebula::storage::UploadChunkAppendStatus::Advanced,
                "retry append should advance chunk index");
    expect_equal(retry_append.next_chunk_index, static_cast<std::int64_t>(1),
                 "retry append should advance next chunk index once");
    expect_equal(nebula::testsupport::read_all(temp_abs_path), std::string("complete"),
                 "retry append should not duplicate partially failed bytes");
}

void test_storage_ref_count_delete_and_gc() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-ref-count-files");
    const TempDir secret_dir("nebula-storage-ref-count-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string complete1 =
        upload_file_single_chunk(server.listening_port(), "/same/a.txt", "same-content", token);
    const std::string complete2 =
        upload_file_single_chunk(server.listening_port(), "/same/b.txt", "same-content", token);

    const std::optional<std::string> sha1 = extract_api_data_string_field(complete1, "sha256");
    const std::optional<std::string> sha2 = extract_api_data_string_field(complete2, "sha256");
    expect_true(sha1.has_value() && sha2.has_value(), "complete should return sha256");
    if (!sha1.has_value() || !sha2.has_value()) {
        nebula::testsupport::fail("complete should return sha256");
    }
    expect_equal(*sha1, *sha2, "same content should deduplicate to same sha");

    const std::optional<std::int64_t> ref_count_2 = fetch_object_ref_count(db_config, *sha1);
    expect_true(ref_count_2.has_value(), "object should exist after two uploads");
    if (!ref_count_2.has_value()) {
        nebula::testsupport::fail("object should exist after two uploads");
    }
    expect_equal(*ref_count_2, static_cast<std::int64_t>(2), "ref_count should be 2 after two uploads");

    const std::string path_a_b64 = nebula::common::base64url_encode("/same/a.txt");
    const std::string delete_a_request =
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", path_a_b64), "", "application/json", token);
    const std::string delete_a_response = send_single_request(server.listening_port(), delete_a_request);
    expect_contains(delete_a_response, "HTTP/1.1 200 OK", "delete a should return 200");

    const std::optional<std::int64_t> ref_count_1 = fetch_object_ref_count(db_config, *sha1);
    expect_true(ref_count_1.has_value(), "object should still exist after deleting one path");
    if (!ref_count_1.has_value()) {
        nebula::testsupport::fail("object should still exist after deleting one path");
    }
    expect_equal(*ref_count_1, static_cast<std::int64_t>(1), "ref_count should be 1 after deleting one path");

    const std::string path_b_b64 = nebula::common::base64url_encode("/same/b.txt");
    const std::string delete_b_request =
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", path_b_b64), "", "application/json", token);
    const std::string delete_b_response = send_single_request(server.listening_port(), delete_b_request);
    expect_contains(delete_b_response, "HTTP/1.1 200 OK", "delete b should return 200");

    const std::string gc_request = build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = send_single_request(server.listening_port(), gc_request);
    expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");

    expect_equal(fetch_object_count(db_config), static_cast<std::int64_t>(0),
                 "all object rows should be cleaned after delete and gc");
}

void test_storage_gc_cleans_file_only_objects() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-file-only-orphan-files");
    const TempDir secret_dir("nebula-storage-file-only-orphan-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string complete_response_body =
        upload_file_single_chunk(server.listening_port(), "/gc/file-only-orphan.txt", "orphan-content", token);
    const std::optional<std::string> sha256 = extract_api_data_string_field(complete_response_body, "sha256");
    expect_true(sha256.has_value(), "upload complete should return sha256");
    if (!sha256.has_value()) {
        nebula::testsupport::fail("upload complete should return sha256");
    }

    const std::filesystem::path object_file_path =
        config.storage_root_dir / "objects" / sha256->substr(0, 2) / sha256->substr(2, 2) / *sha256;
    expect_true(std::filesystem::exists(object_file_path), "object file should exist before gc");

    {
        pqxx::connection connection(nebula::common::build_connection_info(db_config));
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM storage_nodes WHERE path = $1", std::string("/users/1/gc/file-only-orphan.txt"));
        tx.exec_params("DELETE FROM storage_objects WHERE sha256 = $1", *sha256);
        tx.commit();
    }
    expect_equal(fetch_object_count(db_config), static_cast<std::int64_t>(0), "object table should be empty before gc");
    expect_true(std::filesystem::exists(object_file_path), "file-only orphan object should exist before gc");
    {
        std::error_code ec;
        const auto stale_time = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(120);
        std::filesystem::last_write_time(object_file_path, stale_time, ec);
        expect_true(!ec, "set file-only orphan object mtime should succeed");
    }

    const std::string gc_request = build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = send_single_request(server.listening_port(), gc_request);
    expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");
    expect_contains(gc_response, R"("file_only_objects":1)", "gc should report one file-only object");
    expect_contains(gc_response, R"("cleaned_file_only_objects":1)", "gc should clean one file-only object");
    expect_true(!std::filesystem::exists(object_file_path), "file-only orphan object should be removed by gc");
}

void test_storage_gc_cleans_orphan_temp_files() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-temp-orphan-files");
    const TempDir secret_dir("nebula-storage-temp-orphan-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::filesystem::path no_db_temp = config.storage_root_dir / "temp/no-db.part";
    const std::filesystem::path deleted_session_temp = config.storage_root_dir / "temp/deleted-session.part";
    const std::filesystem::path active_temp = config.storage_root_dir / "temp/active-session.part";
    write_test_file(no_db_temp, "no-db");
    write_test_file(deleted_session_temp, "deleted-session");
    write_test_file(active_temp, "active");
    make_file_stale(no_db_temp);
    make_file_stale(deleted_session_temp);
    make_file_stale(active_temp);

    const nebula::storage::UploadSessionCreateResult deleted_create_result = nebula::storage::create_upload_session(
        nebula::storage::UploadSessionRecord{
            .upload_id = "deleted-temp-session",
            .path = "/users/1/deleted-temp.txt",
            .temp_rel_path = "temp/deleted-session.part",
            .total_chunks = 1,
            .next_chunk_index = 0,
            .temp_size_bytes = 0,
        },
        1, nebula::common::now_epoch_s());
    expect_true(deleted_create_result.status == nebula::storage::UploadSessionCreateStatus::Created,
                "deleted temp upload session should be created");
    const nebula::storage::UploadSessionCreateResult active_create_result = nebula::storage::create_upload_session(
        nebula::storage::UploadSessionRecord{
            .upload_id = "active-temp-session",
            .path = "/users/1/active-temp.txt",
            .temp_rel_path = "temp/active-session.part",
            .total_chunks = 1,
            .next_chunk_index = 0,
            .temp_size_bytes = 0,
        },
        1, nebula::common::now_epoch_s());
    expect_true(active_create_result.status == nebula::storage::UploadSessionCreateStatus::Created,
                "active temp upload session should be created");

    {
        pqxx::connection connection(nebula::common::build_connection_info(db_config));
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM storage_upload_sessions WHERE upload_id = $1", std::string("deleted-temp-session"));
        tx.commit();
    }

    const std::string gc_request = build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = send_single_request(server.listening_port(), gc_request);
    expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");
    expect_contains(gc_response, R"("orphan_temp_files":2)", "gc should report two orphan temp files");
    expect_contains(gc_response, R"("cleaned_orphan_temp_files":2)", "gc should clean two orphan temp files");
    expect_contains(gc_response, R"("cleaned_temp_files":2)", "gc should include orphan temp files in cleanup total");
    expect_true(!std::filesystem::exists(no_db_temp), "temp file without db row should be removed by gc");
    expect_true(!std::filesystem::exists(deleted_session_temp), "temp file for deleted db row should be removed by gc");
    expect_true(std::filesystem::exists(active_temp), "active temp file should not be removed by gc");
}

void test_storage_temp_cleanup_waits_for_pending_session_reference() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-temp-cleanup-race-files");

    nebula::server::ServerConfig config;
    apply_database_config(config, db_config);
    config.storage_root_dir = files_dir.path() / "files";
    truncate_storage_tables(db_config);
    ensure_database_pool_initialized(config);

    const nebula::storage::StorageRouteConfig route_config{
        .root_dir = config.storage_root_dir,
        .temp_dir = config.storage_root_dir / "temp",
        .objects_dir = config.storage_root_dir / "objects",
        .upload_session_ttl_s = 86400,
        .max_body_bytes = static_cast<std::size_t>(1024U) * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };

    const std::string temp_rel_path = "temp/pending-session.part";
    const std::filesystem::path temp_abs_path = route_config.root_dir / temp_rel_path;
    write_test_file(temp_abs_path, "seed");

    pqxx::connection blocker_connection(nebula::common::build_connection_info(db_config));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock(hashtextextended($1::text, 0))",
                           std::format("storage_temp:{}", temp_rel_path));
    blocker_tx.exec_params(
        "INSERT INTO storage_upload_sessions(upload_id, path, temp_rel_path, total_chunks, next_chunk_index, "
        "temp_size_bytes, created_at_s, updated_at_s) VALUES($1, $2, $3, 1, 0, $4, $5, $5)",
        std::string("pending-temp-session"), std::string("/users/1/pending-temp.txt"), temp_rel_path,
        static_cast<std::int64_t>(4), static_cast<std::int64_t>(1000));

    auto cleanup_future = std::async(std::launch::async, [&route_config, &temp_abs_path]() {
        return nebula::storage::cleanup_temp_file(route_config, temp_abs_path);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "temp cleanup should wait for pending upload session reference");

    blocker_tx.commit();
    const nebula::storage::CleanupStatus cleanup_status = cleanup_future.get();

    expect_true(cleanup_status == nebula::storage::CleanupStatus::Skipped,
                "temp cleanup should skip after pending session commits");
    expect_true(std::filesystem::exists(temp_abs_path),
                "temp cleanup should keep temp file after pending session commits");
}

void test_storage_unreferenced_cleanup_avoids_resurrection_race() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-cleanup-race-files");
    const TempDir secret_dir("nebula-storage-cleanup-race-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);
    ensure_database_pool_initialized(config);

    const nebula::storage::StorageRouteConfig route_config{
        .root_dir = config.storage_root_dir,
        .temp_dir = config.storage_root_dir / "temp",
        .objects_dir = config.storage_root_dir / "objects",
        .upload_session_ttl_s = 86400,
        .max_body_bytes = static_cast<std::size_t>(1024U) * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
    std::filesystem::create_directories(route_config.objects_dir);

    const std::string sha256(64U, 'a');
    const std::string object_rel_path = std::format("objects/aa/aa/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    std::filesystem::create_directories(object_abs_path.parent_path());
    {
        std::ofstream out(object_abs_path, std::ios::binary);
        out << "seed";
    }
    expect_true(std::filesystem::exists(object_abs_path), "object file should exist before cleanup race test");

    {
        pqxx::connection connection(nebula::common::build_connection_info(db_config));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 0, $4, $4)",
            sha256, static_cast<std::int64_t>(4), object_rel_path, static_cast<std::int64_t>(1000));
        tx.commit();
    }

    pqxx::connection blocker_connection(nebula::common::build_connection_info(db_config));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)", nebula::storage::kStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params("UPDATE storage_objects SET ref_count = 1, updated_at_s = 1001 WHERE sha256 = $1", sha256);
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/race.txt"), sha256, static_cast<std::int64_t>(4),
                           static_cast<std::int64_t>(1001));

    auto cleanup_future = std::async(std::launch::async, [&route_config, &sha256]() {
        nebula::storage::cleanup_unreferenced_object(route_config, sha256);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "cleanup should block while shared advisory lock holder is active");

    blocker_tx.commit();
    cleanup_future.get();

    expect_true(std::filesystem::exists(object_abs_path),
                "cleanup should not delete resurrected referenced object file");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(db_config, sha256);
    expect_true(ref_count.has_value(), "object row should exist after reference resurrection");
    if (!ref_count.has_value()) {
        nebula::testsupport::fail("object row should exist after reference resurrection");
    }
    expect_equal(*ref_count, static_cast<std::int64_t>(1), "ref_count should be 1 after resurrection");
}

void test_storage_upload_failure_cleanup_waits_for_pending_reference() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-upload-cleanup-race-files");

    nebula::server::ServerConfig config;
    apply_database_config(config, db_config);
    config.storage_root_dir = files_dir.path() / "files";
    truncate_storage_tables(db_config);
    ensure_database_pool_initialized(config);

    const nebula::storage::StorageRouteConfig route_config{
        .root_dir = config.storage_root_dir,
        .temp_dir = config.storage_root_dir / "temp",
        .objects_dir = config.storage_root_dir / "objects",
        .upload_session_ttl_s = 86400,
        .max_body_bytes = static_cast<std::size_t>(1024U) * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };

    const std::string sha256(64U, 'c');
    const std::string object_rel_path = std::format("objects/cc/cc/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    write_test_file(object_abs_path, "seed");

    pqxx::connection blocker_connection(nebula::common::build_connection_info(db_config));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)", nebula::storage::kStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params(
        "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
        "VALUES($1, $2, $3, 1, $4, $4)",
        sha256, static_cast<std::int64_t>(4), object_rel_path, static_cast<std::int64_t>(1000));
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/pending-reference.txt"), sha256, static_cast<std::int64_t>(4),
                           static_cast<std::int64_t>(1000));

    auto cleanup_future = std::async(std::launch::async, [&route_config, &sha256, &object_abs_path]() {
        nebula::storage::cleanup_upload_failure_object(route_config, "failed-upload", sha256, object_abs_path);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "upload failure cleanup should wait for pending object reference");

    blocker_tx.commit();
    cleanup_future.get();

    expect_true(std::filesystem::exists(object_abs_path),
                "upload failure cleanup should keep object file after pending reference commits");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(db_config, sha256);
    expect_true(ref_count.has_value(), "pending referenced object row should exist after cleanup");
    if (!ref_count.has_value()) {
        nebula::testsupport::fail("pending referenced object row should exist after cleanup");
    }
    expect_equal(*ref_count, static_cast<std::int64_t>(1), "pending referenced object ref_count should stay 1");
}

void test_storage_file_only_object_cleanup_waits_for_pending_reference() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-file-only-cleanup-race-files");

    nebula::server::ServerConfig config;
    apply_database_config(config, db_config);
    config.storage_root_dir = files_dir.path() / "files";
    truncate_storage_tables(db_config);
    ensure_database_pool_initialized(config);

    const nebula::storage::StorageRouteConfig route_config{
        .root_dir = config.storage_root_dir,
        .temp_dir = config.storage_root_dir / "temp",
        .objects_dir = config.storage_root_dir / "objects",
        .upload_session_ttl_s = 86400,
        .max_body_bytes = static_cast<std::size_t>(1024U) * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };

    const std::string sha256(64U, 'd');
    const std::string object_rel_path = std::format("objects/dd/dd/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    write_test_file(object_abs_path, "seed");

    pqxx::connection blocker_connection(nebula::common::build_connection_info(db_config));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)", nebula::storage::kStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params(
        "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
        "VALUES($1, $2, $3, 1, $4, $4)",
        sha256, static_cast<std::int64_t>(4), object_rel_path, static_cast<std::int64_t>(1000));
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/file-only-pending-reference.txt"), sha256,
                           static_cast<std::int64_t>(4), static_cast<std::int64_t>(1000));

    auto cleanup_future = std::async(std::launch::async, [&route_config, &object_abs_path]() {
        return nebula::storage::cleanup_file_only_object(route_config, object_abs_path);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "file-only object cleanup should wait for pending object reference");

    blocker_tx.commit();
    const nebula::storage::CleanupStatus cleanup_status = cleanup_future.get();

    expect_true(cleanup_status == nebula::storage::CleanupStatus::Skipped,
                "file-only object cleanup should skip after pending reference commits");
    expect_true(std::filesystem::exists(object_abs_path),
                "file-only object cleanup should keep object file after pending reference commits");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(db_config, sha256);
    expect_true(ref_count.has_value(), "pending referenced file-only object row should exist after cleanup");
    if (!ref_count.has_value()) {
        nebula::testsupport::fail("pending referenced file-only object row should exist after cleanup");
    }
    expect_equal(*ref_count, static_cast<std::int64_t>(1),
                 "pending referenced file-only object ref_count should stay 1");
}

void test_storage_tree_and_directory_delete() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-tree-files");
    const TempDir secret_dir("nebula-storage-tree-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/dir/a.txt", "a", token);
    upload_file_single_chunk(server.listening_port(), "/dir/sub/b.txt", "b", token);

    const std::string dir_b64 = nebula::common::base64url_encode("/dir");
    const std::string tree_request =
        build_http_request("GET", std::format("/api/storage/tree/{}", dir_b64), "", "application/json", token);
    const std::string tree_response = send_single_request(server.listening_port(), tree_request);
    expect_contains(tree_response, "HTTP/1.1 200 OK", "tree list should return 200");
    expect_contains(tree_response, R"("name":"a.txt")", "tree should include file item");
    expect_contains(tree_response, R"("name":"sub")", "tree should include sub directory");
    expect_contains(tree_response, R"("type":"directory")", "tree should include directory type");

    const std::string delete_dir_request =
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", dir_b64), "", "application/json", token);
    const std::string delete_dir_response = send_single_request(server.listening_port(), delete_dir_request);
    expect_contains(delete_dir_response, "HTTP/1.1 409 Conflict", "delete non-empty directory should return 409");

    const std::string file_a_b64 = nebula::common::base64url_encode("/dir/a.txt");
    const std::string file_b_b64 = nebula::common::base64url_encode("/dir/sub/b.txt");
    const std::string sub_dir_b64 = nebula::common::base64url_encode("/dir/sub");
    const std::string delete_a_response = send_single_request(
        server.listening_port(),
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", file_a_b64), "", "application/json", token));
    const std::string delete_b_response = send_single_request(
        server.listening_port(),
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", file_b_b64), "", "application/json", token));
    expect_contains(delete_a_response, "HTTP/1.1 200 OK", "delete file a should return 200");
    expect_contains(delete_a_response, R"("type":"file")", "delete file should report file type");
    expect_contains(delete_b_response, "HTTP/1.1 200 OK", "delete file b should return 200");
    const std::string delete_sub_dir_response = send_single_request(
        server.listening_port(),
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", sub_dir_b64), "", "application/json", token));
    expect_contains(delete_sub_dir_response, "HTTP/1.1 200 OK", "delete empty sub directory should return 200");
    expect_contains(delete_sub_dir_response, R"("type":"directory")",
                    "delete empty sub directory should report directory type");

    const std::string delete_dir_empty_response = send_single_request(
        server.listening_port(),
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", dir_b64), "", "application/json", token));
    expect_contains(delete_dir_empty_response, "HTTP/1.1 200 OK", "delete empty directory should return 200");
}

void test_storage_explicit_directory_contracts() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-explicit-dir-files");
    const TempDir secret_dir("nebula-storage-explicit-dir-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    const std::string empty_dir_b64 = nebula::common::base64url_encode("/empty");
    const std::string create_empty_response =
        send_single_request(port, build_http_request("PUT", std::format("/api/storage/directories/{}", empty_dir_b64),
                                                     "", "application/json", token));
    expect_contains(create_empty_response, "HTTP/1.1 200 OK", "create explicit directory should return 200");
    expect_contains(create_empty_response, R"("type":"directory")", "create directory should report directory type");

    const std::string root_b64 = nebula::common::base64url_encode("/");
    const std::string root_tree_response = send_single_request(
        port, build_http_request("GET", std::format("/api/storage/tree/{}", root_b64), "", "application/json", token));
    expect_contains(root_tree_response, "HTTP/1.1 200 OK", "root tree should return 200");
    expect_contains(root_tree_response, R"("name":"empty")", "root tree should include created directory");

    const std::string empty_tree_response = send_single_request(
        port,
        build_http_request("GET", std::format("/api/storage/tree/{}", empty_dir_b64), "", "application/json", token));
    expect_contains(empty_tree_response, "HTTP/1.1 200 OK", "empty directory tree should return 200");
    expect_contains(empty_tree_response, R"("items":[])", "empty directory tree should be empty");

    const std::string create_existing_response =
        send_single_request(port, build_http_request("PUT", std::format("/api/storage/directories/{}", empty_dir_b64),
                                                     "", "application/json", token));
    expect_contains(create_existing_response, "HTTP/1.1 409 Conflict", "creating existing directory should return 409");
    expect_contains(create_existing_response, R"("code":"directory_already_exists")",
                    "creating existing directory should report directory_already_exists");

    const std::string missing_child_b64 = nebula::common::base64url_encode("/missing/child");
    const std::string create_missing_child_response = send_single_request(
        port, build_http_request("PUT", std::format("/api/storage/directories/{}", missing_child_b64), "",
                                 "application/json", token));
    expect_contains(create_missing_child_response, "HTTP/1.1 404 Not Found",
                    "creating child under missing parent should return 404");
    expect_contains(create_missing_child_response, R"("code":"parent_not_found")",
                    "creating child under missing parent should report parent_not_found");

    const std::string missing_b64 = nebula::common::base64url_encode("/missing");
    const std::string missing_tree_response = send_single_request(
        port,
        build_http_request("GET", std::format("/api/storage/tree/{}", missing_b64), "", "application/json", token));
    expect_contains(missing_tree_response, "HTTP/1.1 404 Not Found", "missing directory list should return 404");
    expect_contains(missing_tree_response, R"("code":"path_not_found")",
                    "missing directory list should report path_not_found");

    const std::string delete_missing_response = send_single_request(
        port,
        build_http_request("DELETE", std::format("/api/storage/nodes/{}", missing_b64), "", "application/json", token));
    expect_contains(delete_missing_response, "HTTP/1.1 404 Not Found", "delete missing node should return 404");
    expect_contains(delete_missing_response, R"("code":"path_not_found")",
                    "delete missing node should report path_not_found");

    const std::string download_dir_response = send_single_request(
        port,
        build_http_request("GET", std::format("/api/storage/files/{}", empty_dir_b64), "", "application/json", token));
    expect_contains(download_dir_response, "HTTP/1.1 409 Conflict", "download directory should return 409");
    expect_contains(download_dir_response, R"("code":"not_file")", "download directory should report not_file");

    const std::string missing_file_b64 = nebula::common::base64url_encode("/missing/file.txt");
    const std::string missing_file_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", missing_file_b64);
    const std::string missing_file_init_response = send_single_request(
        port,
        build_http_request("POST", "/api/storage/uploads/init", missing_file_init_body, "application/json", token));
    expect_contains(missing_file_init_response, "HTTP/1.1 404 Not Found",
                    "upload init under missing parent should return 404");
    expect_contains(missing_file_init_response, R"("code":"parent_not_found")",
                    "upload init under missing parent should report parent_not_found");

    const std::string dir_as_file_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", empty_dir_b64);
    const std::string dir_as_file_init_response = send_single_request(
        port,
        build_http_request("POST", "/api/storage/uploads/init", dir_as_file_init_body, "application/json", token));
    expect_contains(dir_as_file_init_response, "HTTP/1.1 409 Conflict", "upload init over directory should return 409");
    expect_contains(dir_as_file_init_response, R"("code":"path_conflict")",
                    "upload init over directory should report path_conflict");
    expect_contains(dir_as_file_init_response,
                    R"("message":"storage path conflicts with an existing file or directory")",
                    "upload init over directory should report user-facing path conflict message");
}

void test_storage_rejects_file_directory_path_collisions() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-tree-collision-files");
    const TempDir secret_dir("nebula-storage-tree-collision-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 4;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    upload_file_single_chunk(port, "/dir/a.txt", "child", token);
    const std::string dir_path_b64 = nebula::common::base64url_encode("/dir");
    const std::string dir_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", dir_path_b64);
    const std::string dir_init_response = send_single_request(
        port, build_http_request("POST", "/api/storage/uploads/init", dir_init_body, "application/json", token));
    expect_contains(dir_init_response, "HTTP/1.1 409 Conflict",
                    "uploading file over directory should return 409 at init");
    expect_contains(dir_init_response, R"("code":"path_conflict")",
                    "uploading file over directory should report path_conflict");

    upload_file_single_chunk(port, "/leaf", "file", token);
    const std::string leaf_child_b64 = nebula::common::base64url_encode("/leaf/a.txt");
    const std::string leaf_child_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", leaf_child_b64);
    const std::string leaf_child_init_response = send_single_request(
        port, build_http_request("POST", "/api/storage/uploads/init", leaf_child_init_body, "application/json", token));
    expect_contains(leaf_child_init_response, "HTTP/1.1 409 Conflict",
                    "uploading child under file should return 409 at init");
    expect_contains(leaf_child_init_response, R"("code":"parent_not_directory")",
                    "uploading child under file should report parent_not_directory");

    const std::string race_file_upload_id = init_and_upload_single_chunk(port, "/race", "file", token);
    auto file_complete_future = std::async(std::launch::async, [port, race_file_upload_id, token]() {
        return complete_upload(port, race_file_upload_id, token);
    });
    auto dir_create_future = std::async(std::launch::async, [port, token]() {
        const std::string race_b64 = nebula::common::base64url_encode("/race");
        return send_single_request(port, build_http_request("PUT", std::format("/api/storage/directories/{}", race_b64),
                                                            "", "application/json", token));
    });
    const std::array<std::string, 2> race_responses{file_complete_future.get(), dir_create_future.get()};
    const auto ok_count = std::count_if(race_responses.begin(), race_responses.end(), [](const std::string& response) {
        return response.find("HTTP/1.1 200 OK") != std::string::npos;
    });
    const auto conflict_count =
        std::count_if(race_responses.begin(), race_responses.end(), [](const std::string& response) {
            return response.find("HTTP/1.1 409 Conflict") != std::string::npos &&
                   (response.find(R"("code":"path_conflict")") != std::string::npos ||
                    response.find(R"("code":"directory_already_exists")") != std::string::npos);
        });
    expect_equal(static_cast<std::int64_t>(ok_count), static_cast<std::int64_t>(1),
                 "exactly one concurrent conflicting file or directory create should complete");
    expect_equal(static_cast<std::int64_t>(conflict_count), static_cast<std::int64_t>(1),
                 "exactly one concurrent conflicting operation should return conflict");

    pqxx::connection connection(nebula::common::build_connection_info(db_config));
    pqxx::read_transaction tx(connection);
    const auto race_node_count =
        tx.exec_params("SELECT COUNT(*) FROM storage_nodes WHERE path = $1", std::string("/users/1/race"))[0][0]
            .as<std::int64_t>(0);
    expect_equal(race_node_count, static_cast<std::int64_t>(1),
                 "concurrent file/directory race should leave one tree node at the path");
}

void test_storage_rejects_files_above_size_limit() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-size-limit-files");
    const TempDir secret_dir("nebula-storage-size-limit-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.storage_max_file_bytes = 5;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path_b64 = nebula::common::base64url_encode("/limited.bin");
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":2}})", path_b64);
    const std::string init_response = send_single_request(
        server.listening_port(),
        build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token));
    expect_contains(init_response, "HTTP/1.1 200 OK", "limited upload init should return 200");
    const std::optional<std::string> upload_id =
        extract_api_data_string_field(response_body(init_response), "upload_id");
    expect_true(upload_id.has_value(), "limited upload init should return upload_id");
    if (!upload_id.has_value()) {
        nebula::testsupport::fail("limited upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_response = send_single_request(
        server.listening_port(), build_http_request("PUT", chunk0_path, "abc", "application/octet-stream", token));
    expect_contains(chunk0_response, "HTTP/1.1 200 OK", "first limited chunk should fit");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_response = send_single_request(
        server.listening_port(), build_http_request("PUT", chunk1_path, "def", "application/octet-stream", token));
    expect_contains(chunk1_response, "HTTP/1.1 413 Content Too Large", "oversized accumulated file should return 413");
    expect_contains(chunk1_response, R"("code":"file_too_large")", "oversized accumulated file should use fixed code");

    const std::string complete_response = send_single_request(
        server.listening_port(), build_http_request("POST", std::format("/api/storage/uploads/{}/complete", *upload_id),
                                                    "", "application/json", token));
    expect_contains(complete_response, "HTTP/1.1 409 Conflict", "rejected oversized chunk should not advance upload");

    const std::string sha256(64U, 'a');
    const std::string object_rel_path =
        std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
    const std::filesystem::path object_abs_path = config.storage_root_dir / object_rel_path;
    std::filesystem::create_directories(object_abs_path.parent_path());
    {
        std::ofstream object_file(object_abs_path, std::ios::binary);
        object_file << "123456";
    }
    {
        pqxx::connection connection(nebula::common::build_connection_info(db_config));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 1, 1000, 1000)",
            sha256, static_cast<std::int64_t>(5), object_rel_path);
        tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, 1000)",
                       std::string("/users/1/manual-too-large.bin"), sha256, static_cast<std::int64_t>(5));
        tx.commit();
    }

    const std::string manual_path_b64 = nebula::common::base64url_encode("/manual-too-large.bin");
    const std::string download_response = send_single_request(
        server.listening_port(), build_http_request("GET", std::format("/api/storage/files/{}", manual_path_b64), "",
                                                    "application/json", token));
    expect_contains(download_response, "HTTP/1.1 413 Content Too Large",
                    "download should reject object file above size limit");
    expect_contains(download_response, R"("code":"file_too_large")", "download size limit should use fixed code");
}

void test_storage_prefix_wildcards_match_literals() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-prefix-wildcard-files");
    const TempDir secret_dir("nebula-storage-prefix-wildcard-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/literal%/inside.txt", "percent", token);
    upload_file_single_chunk(server.listening_port(), "/literalX/outside.txt", "x", token);
    upload_file_single_chunk(server.listening_port(), "/literal_/inside.txt", "underscore", token);
    upload_file_single_chunk(server.listening_port(), "/literala/outside.txt", "a", token);

    const std::string percent_tree_b64 = nebula::common::base64url_encode("/literal%");
    const std::string percent_tree_response = send_single_request(
        server.listening_port(), build_http_request("GET", std::format("/api/storage/tree/{}", percent_tree_b64), "",
                                                    "application/json", token));
    expect_contains(percent_tree_response, "HTTP/1.1 200 OK", "literal percent tree query should succeed");
    expect_contains(percent_tree_response, R"("name":"inside.txt")",
                    "literal percent tree should return literal child");
    expect_not_contains(percent_tree_response, R"("outside.txt")",
                        "literal percent tree should not match wildcard siblings");

    const std::string underscore_tree_b64 = nebula::common::base64url_encode("/literal_");
    const std::string underscore_tree_response = send_single_request(
        server.listening_port(), build_http_request("GET", std::format("/api/storage/tree/{}", underscore_tree_b64), "",
                                                    "application/json", token));
    expect_contains(underscore_tree_response, "HTTP/1.1 200 OK", "literal underscore tree query should succeed");
    expect_contains(underscore_tree_response, R"("name":"inside.txt")",
                    "literal underscore tree should return literal child");
    expect_not_contains(underscore_tree_response, R"("outside.txt")",
                        "literal underscore tree should not match wildcard siblings");

    const std::string percent_file_b64 = nebula::common::base64url_encode("/literal%/inside.txt");
    const std::string percent_dir_b64 = nebula::common::base64url_encode("/literal%");
    const std::string delete_percent_file_response = send_single_request(
        server.listening_port(), build_http_request("DELETE", std::format("/api/storage/nodes/{}", percent_file_b64),
                                                    "", "application/json", token));
    expect_contains(delete_percent_file_response, "HTTP/1.1 200 OK", "delete literal percent file should return 200");

    const std::string delete_percent_dir_response = send_single_request(
        server.listening_port(), build_http_request("DELETE", std::format("/api/storage/nodes/{}", percent_dir_b64), "",
                                                    "application/json", token));
    expect_contains(delete_percent_dir_response, "HTTP/1.1 200 OK",
                    "empty literal percent dir should ignore wildcard siblings");
}

void test_storage_isolated_between_users() {
    const PostgresConnectionPoolOptions db_config = require_postgres_pool_test_options();
    const TempDir files_dir("nebula-storage-isolation-files");
    const TempDir secret_dir("nebula-storage-isolation-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    write_jwt_secret_file(secret_path, kIntegrationJwtSecret);

    nebula::server::ServerConfig config;
    config.port = 0;
    config.worker_thread_count = 2;
    config.auth_jwt_secret_path = secret_path;
    config.storage_root_dir = files_dir.path() / "files";
    apply_database_config(config, db_config);
    truncate_storage_tables(db_config);

    auto storage_runtime = build_storage_router(config);
    auto server = build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    std::thread server_thread([&server]() { server.run(); });
    ServerThreadGuard server_guard(server, server_thread);
    wait_until_server_ready(server);

    const std::string token_a = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");
    const std::string token_b = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/private/a.txt", "hello-from-alice", token_a);

    const std::string path_b64 = nebula::common::base64url_encode("/private/a.txt");
    const std::string download_by_a = send_single_request(
        server.listening_port(),
        build_http_request("GET", std::format("/api/storage/files/{}", path_b64), "", "application/json", token_a));
    expect_contains(download_by_a, "HTTP/1.1 200 OK", "owner download should return 200");

    const std::string download_by_b = send_single_request(
        server.listening_port(),
        build_http_request("GET", std::format("/api/storage/files/{}", path_b64), "", "application/json", token_b));
    expect_contains(download_by_b, "HTTP/1.1 404 Not Found", "other user download should return 404");

    const std::string root_b64 = nebula::common::base64url_encode("/");
    const std::string tree_by_b = send_single_request(
        server.listening_port(),
        build_http_request("GET", std::format("/api/storage/tree/{}", root_b64), "", "application/json", token_b));
    expect_contains(tree_by_b, "HTTP/1.1 200 OK", "other user tree list should return 200");
    expect_not_contains(tree_by_b, R"("private")", "other user tree should not include first user folder");
}

int run_http_server_storage_db_integration_tests() {
    const TempDir log_dir("nebula-http-server-storage-db-integration-log");
    nebula::common::Logger::instance().initialize(nebula::common::LogLevel::Warning, log_dir.path(), false);

    const std::optional<std::string> env_error = nebula::testsupport::database::validate_database_test_env();
    if (env_error.has_value()) {
        std::cerr << "[SKIP] http server storage db integration precheck skipped: error=" << *env_error << '\n';
        return nebula::testsupport::kTestSkipReturnCode;
    }

    const std::vector<nebula::testsupport::TestCase> tests = {
        {"storage node schema constraints", test_storage_node_schema_constraints},
        {"storage requires access token", test_storage_requires_access_token},
        {"storage rejects non canonical user paths", test_storage_rejects_non_canonical_user_paths},
        {"storage upload complete download flow", test_storage_upload_complete_download_flow},
        {"storage upload rejects chunk after expected count", test_storage_upload_rejects_chunk_after_expected_count},
        {"storage upload chunk failure restores db size", test_storage_upload_chunk_failure_restores_db_size},
        {"storage rejects files above size limit", test_storage_rejects_files_above_size_limit},
        {"storage ref count delete and gc", test_storage_ref_count_delete_and_gc},
        {"storage gc cleans file-only objects", test_storage_gc_cleans_file_only_objects},
        {"storage gc cleans orphan temp files", test_storage_gc_cleans_orphan_temp_files},
        {"storage temp cleanup waits for pending session reference",
         test_storage_temp_cleanup_waits_for_pending_session_reference},
        {"storage unreferenced cleanup avoids resurrection race",
         test_storage_unreferenced_cleanup_avoids_resurrection_race},
        {"storage upload failure cleanup waits for pending reference",
         test_storage_upload_failure_cleanup_waits_for_pending_reference},
        {"storage file-only object cleanup waits for pending reference",
         test_storage_file_only_object_cleanup_waits_for_pending_reference},
        {"storage tree and directory delete", test_storage_tree_and_directory_delete},
        {"storage explicit directory contracts", test_storage_explicit_directory_contracts},
        {"storage rejects file directory path collisions", test_storage_rejects_file_directory_path_collisions},
        {"storage prefix wildcards match literals", test_storage_prefix_wildcards_match_literals},
        {"storage isolated between users", test_storage_isolated_between_users},
    };
    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_http_server_storage_db_integration_tests);
}
