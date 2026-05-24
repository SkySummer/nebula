#include "nebula_tests/http.hpp"

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
#include <limits>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nebula/auth/bootstrap/module.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula/common/codec/json.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/security/crypto.hpp"
#include "nebula/database/config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/storage/application/service.hpp"
#include "nebula/storage/http/routes.hpp"
#include "nebula/storage/infra/object_store.hpp"
#include "nebula/storage/repository/repository.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/database.hpp"
#include "nebula_tests/integration.hpp"

namespace {

using namespace nebula;

struct StorageRouteRuntime {
    std::shared_ptr<nebula::database::ConnectionPool> database_pool;
    std::shared_ptr<nebula::storage::ObjectStore> object_store;
    std::shared_ptr<nebula::storage::StorageRepository> storage_repository;
    std::shared_ptr<nebula::storage::StorageService> storage_service;
    std::shared_ptr<nebula::http::Router> router;
    std::shared_ptr<nebula::auth::AuthService> auth_service;
};

StorageRouteRuntime build_storage_router(const nebula::app::AppConfig& config) {
    StorageRouteRuntime runtime;
    runtime.database_pool = test::database::create_database_pool(config.database);
    runtime.storage_repository = std::make_shared<nebula::storage::StorageRepository>(runtime.database_pool);
    nebula::storage::StorageRuntimeConfig route_config;
    route_config.root_dir = config.storage.root_dir;
    route_config.temp_dir = route_config.root_dir / "temp";
    route_config.objects_dir = route_config.root_dir / "objects";
    route_config.upload_session_ttl = std::chrono::seconds{config.storage.upload_session_ttl_s};
    route_config.download_ticket_ttl = std::chrono::seconds{config.storage.download_ticket_ttl_s};
    route_config.max_body_bytes = config.limits.max_body_bytes;
    route_config.max_file_bytes = config.storage.max_file_bytes;
    runtime.object_store = std::make_shared<nebula::storage::ObjectStore>(route_config);
    test::expect_true(runtime.object_store->ensure_root_dirs(), "create storage object store should succeed");
    runtime.storage_service = std::make_shared<nebula::storage::StorageService>(runtime.storage_repository,
                                                                                runtime.object_store, route_config);
    runtime.router = std::make_shared<nebula::http::Router>();
    const auto module = nebula::auth::AuthModule::create(nebula::auth::AuthModule::Params{
        .config = &config.auth,
        .database_pool = runtime.database_pool,
        .router = runtime.router,
    });
    test::expect_true(module != nullptr, "create auth module should succeed");
    runtime.auth_service = module->service();
    test::expect_true(runtime.auth_service != nullptr, "create auth service should succeed");
    test::expect_true(nebula::storage::register_storage_routes(runtime.router, runtime.storage_service),
                      "register storage routes should succeed");
    return runtime;
}

struct ParsedTreeItems {
    std::vector<std::string> names;
    std::vector<std::int64_t> updated_at_values;
    std::unordered_map<std::string, std::string> node_types_by_name;
    std::unordered_map<std::string, std::string> file_types_by_name;
    std::unordered_map<std::string, std::int64_t> file_counts_by_name;
    std::unordered_map<std::string, std::string> paths_by_name;
};

struct UsageBreakdownItem {
    std::string file_type;
    std::int64_t size_bytes = 0;
    std::int64_t file_count = 0;
};

struct ParsedUsageResponse {
    std::int64_t total_bytes = 0;
    std::int64_t used_bytes = 0;
    std::int64_t available_bytes = 0;
    std::int64_t used_percent = 0;
    std::int64_t max_chunk_bytes = 0;
    std::int64_t max_file_bytes = 0;
    std::vector<UsageBreakdownItem> breakdown;
};

const nebula::common::JsonObject& require_json_object(const nebula::common::JsonValue& value, std::string_view context,
                                                      std::string_view label) {
    const nebula::common::JsonObject* object = value.get_if_object();
    if (object == nullptr) {
        nebula::test::fail(std::format("{} {} should be a json object", context, label));
    }
    return *object;
}

const nebula::common::JsonObject& require_object_member_object(const nebula::common::JsonObject& object,
                                                               std::string_view key, std::string_view context) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        nebula::test::fail(std::format("{} should contain {} object", context, key));
    }
    return require_json_object(it->second, context, key);
}

const nebula::common::JsonArray& require_object_member_array(const nebula::common::JsonObject& object,
                                                             std::string_view key, std::string_view context) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        nebula::test::fail(std::format("{} should contain {}", context, key));
    }
    const nebula::common::JsonArray* array = it->second.get_if_array();
    if (array == nullptr) {
        nebula::test::fail(std::format("{} {} should be an array", context, key));
    }
    return *array;
}

std::string require_object_member_string(const nebula::common::JsonObject& object, std::string_view key,
                                         std::string_view context) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        nebula::test::fail(std::format("{} item should contain {}", context, key));
    }
    const std::string* value = it->second.get_if_string();
    if (value == nullptr) {
        nebula::test::fail(std::format("{} {} should be string", context, key));
    }
    return *value;
}

std::int64_t require_object_member_int64(const nebula::common::JsonObject& object, std::string_view key,
                                         std::string_view context) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        nebula::test::fail(std::format("{} item should contain {}", context, key));
    }
    const std::int64_t* value = it->second.get_if_int64();
    if (value == nullptr) {
        nebula::test::fail(std::format("{} {} should be int64", context, key));
    }
    return *value;
}

std::optional<std::string> optional_object_member_string(const nebula::common::JsonObject& object,
                                                         std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return std::nullopt;
    }
    const std::string* value = it->second.get_if_string();
    if (value == nullptr) {
        nebula::test::fail(std::format("{} should be string when present", key));
    }
    return *value;
}

std::optional<std::int64_t> optional_object_member_int64(const nebula::common::JsonObject& object,
                                                         std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return std::nullopt;
    }
    const std::int64_t* value = it->second.get_if_int64();
    if (value == nullptr) {
        nebula::test::fail(std::format("{} should be int64 when present", key));
    }
    return *value;
}

ParsedTreeItems parse_tree_items_response(std::string_view json, std::string_view context) {
    const nebula::common::JsonParseResult parsed = nebula::common::parse_json(json);
    if (!parsed.ok) {
        nebula::test::fail(std::format("{} should parse as json", context));
    }

    const nebula::common::JsonObject& root = require_json_object(parsed.value, context, "root");
    const nebula::common::JsonObject& data = require_object_member_object(root, "data", context);
    const nebula::common::JsonArray& items = require_object_member_array(data, "items", context);

    ParsedTreeItems parsed_items;
    parsed_items.names.reserve(items.size());
    parsed_items.updated_at_values.reserve(items.size());
    for (const nebula::common::JsonValue& item_value : items) {
        const nebula::common::JsonObject& item = require_json_object(item_value, context, "item");
        const std::string name = require_object_member_string(item, "name", context);
        parsed_items.names.push_back(name);
        parsed_items.updated_at_values.push_back(require_object_member_int64(item, "updated_at", context));
        const std::optional<std::string> node_type = optional_object_member_string(item, "type");
        if (node_type.has_value()) {
            parsed_items.node_types_by_name.emplace(name, *node_type);
        }
        const std::optional<std::string> file_type = optional_object_member_string(item, "file_type");
        if (file_type.has_value()) {
            parsed_items.file_types_by_name.emplace(name, *file_type);
        }
        const std::optional<std::int64_t> file_count = optional_object_member_int64(item, "file_count");
        if (file_count.has_value()) {
            parsed_items.file_counts_by_name.emplace(name, *file_count);
        }
        const std::optional<std::string> path = optional_object_member_string(item, "path");
        if (path.has_value()) {
            parsed_items.paths_by_name.emplace(name, *path);
        }
    }
    return parsed_items;
}

ParsedUsageResponse parse_usage_response(std::string_view json, std::string_view context) {
    const nebula::common::JsonParseResult parsed = nebula::common::parse_json(json);
    if (!parsed.ok) {
        nebula::test::fail(std::format("{} should parse as json", context));
    }

    const nebula::common::JsonObject& root = require_json_object(parsed.value, context, "root");
    const nebula::common::JsonObject& data = require_object_member_object(root, "data", context);
    const nebula::common::JsonArray& breakdown = require_object_member_array(data, "breakdown", context);

    ParsedUsageResponse response;
    response.total_bytes = require_object_member_int64(data, "total_bytes", context);
    response.used_bytes = require_object_member_int64(data, "used_bytes", context);
    response.available_bytes = require_object_member_int64(data, "available_bytes", context);
    response.used_percent = require_object_member_int64(data, "used_percent", context);
    response.max_chunk_bytes = require_object_member_int64(data, "max_chunk_bytes", context);
    response.max_file_bytes = require_object_member_int64(data, "max_file_bytes", context);
    response.breakdown.reserve(breakdown.size());
    for (const nebula::common::JsonValue& item_value : breakdown) {
        const nebula::common::JsonObject& item = require_json_object(item_value, context, "breakdown_item");
        response.breakdown.push_back(UsageBreakdownItem{
            .file_type = require_object_member_string(item, "file_type", context),
            .size_bytes = require_object_member_int64(item, "size_bytes", context),
            .file_count = require_object_member_int64(item, "file_count", context),
        });
    }

    return response;
}

std::optional<std::int64_t> fetch_object_ref_count(const database::DatabaseConfig& config, std::string_view sha256) {
    pqxx::connection connection(database::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    const pqxx::result rows =
        tx.exec_params("SELECT ref_count FROM storage_objects WHERE sha256 = $1 LIMIT 1", std::string(sha256));
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front()[0].as<std::int64_t>(0);
}

std::int64_t fetch_object_count(const database::DatabaseConfig& config) {
    pqxx::connection connection(database::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    return tx.exec1("SELECT COUNT(*) FROM storage_objects")[0].as<std::int64_t>(0);
}

struct StoredDownloadTicketRow {
    std::int64_t user_id = 0;
    std::string canonical_path;
    std::int64_t expires_at_s = 0;
};

std::optional<std::string> extract_download_ticket_token(std::string_view download_url) {
    static constexpr std::string_view kPrefix = "/api/storage/downloads/";
    if (!download_url.starts_with(kPrefix) || download_url.size() <= kPrefix.size()) {
        return std::nullopt;
    }
    return std::string(download_url.substr(kPrefix.size()));
}

std::optional<StoredDownloadTicketRow> fetch_download_ticket_row(const database::DatabaseConfig& config,
                                                                 std::string_view ticket) {
    pqxx::connection connection(database::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    const pqxx::result rows = tx.exec_params(
        "SELECT user_id, canonical_path, expires_at_s FROM storage_download_tickets WHERE ticket = $1 LIMIT 1",
        std::string(ticket));
    if (rows.empty()) {
        return std::nullopt;
    }

    const pqxx::row row = rows.front();
    return StoredDownloadTicketRow{
        .user_id = row[0].as<std::int64_t>(0),
        .canonical_path = row[1].as<std::string>(),
        .expires_at_s = row[2].as<std::int64_t>(0),
    };
}

std::int64_t fetch_download_ticket_count(const database::DatabaseConfig& config) {
    pqxx::connection connection(database::build_connection_info(config));
    pqxx::read_transaction tx(connection);
    return tx.exec1("SELECT COUNT(*) FROM storage_download_tickets")[0].as<std::int64_t>(0);
}

template <typename Write>
void expect_storage_node_write_rejected(const database::DatabaseConfig& config, Write write, std::string_view message) {
    try {
        pqxx::connection connection(database::build_connection_info(config));
        pqxx::work tx(connection);
        write(tx);
        tx.commit();
        nebula::test::fail(message);
    } catch (const pqxx::sql_error& e) {
        test::expect_equal(e.sqlstate(), std::string("23514"), message);
    }
}

void make_file_stale(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stale_time = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(120);
    std::filesystem::last_write_time(path, stale_time, ec);
    test::expect_true(!ec, "set stale file mtime should succeed");
}

std::string register_user_and_get_token(std::uint16_t port, std::string_view username, std::string_view password) {
    const std::string register_body =
        std::format(R"({{"username":"{}","password":"{}"}})", std::string(username), std::string(password));
    const std::string register_request =
        test::http::build_http_request("POST", "/api/auth/register", register_body, "application/json");
    const std::string register_response = test::http::send_single_request(port, register_request);
    test::expect_contains(register_response, "HTTP/1.1 200 OK", "register should return 200");
    const std::optional<std::string> token =
        test::http::extract_api_data_string_field(test::http::response_body(register_response), "access_token");
    if (!token.has_value() || token->empty()) {
        test::fail("register should return access token");
    }
    return *token;
}

std::string init_and_upload_single_chunk(std::uint16_t port, std::string_view path, std::string_view content,
                                         std::string_view access_token) {
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_request = test::http::build_http_request("POST", "/api/storage/uploads/init", init_body,
                                                                    "application/json", access_token);
    const std::string init_response = test::http::send_single_request(port, init_request);
    test::expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");

    const std::optional<std::string> upload_id =
        test::http::extract_api_data_string_field(test::http::response_body(init_response), "upload_id");
    if (!upload_id.has_value()) {
        test::fail("upload init should return upload_id");
    }

    const std::string chunk_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk_request =
        test::http::build_http_request("PUT", chunk_path, content, "application/octet-stream", access_token);
    const std::string chunk_response = test::http::send_single_request(port, chunk_request);
    test::expect_contains(chunk_response, "HTTP/1.1 200 OK", "upload chunk should return 200");

    return *upload_id;
}

std::string complete_upload(std::uint16_t port, std::string_view upload_id, std::string_view access_token) {
    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", upload_id);
    const std::string complete_request =
        test::http::build_http_request("POST", complete_path, "", "application/json", access_token);
    return test::http::send_single_request(port, complete_request);
}

void ensure_directory_exists(std::uint16_t port, std::string_view path, std::string_view access_token) {
    if (path == "/") {
        return;
    }

    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string response = test::http::send_single_request(
        port, test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", path_b64), "",
                                             "application/json", access_token));
    const bool created = response.find("HTTP/1.1 200 OK") != std::string::npos;
    const bool already_exists = response.find("HTTP/1.1 409 Conflict") != std::string::npos &&
                                response.find(R"("code":"directory_already_exists")") != std::string::npos;
    test::expect_true(created || already_exists, "directory setup should create or find existing directory");
}

void ensure_parent_directories(std::uint16_t port, std::string_view file_path, std::string_view access_token) {
    for (std::size_t slash_pos = file_path.find('/', 1); slash_pos != std::string_view::npos;
         slash_pos = file_path.find('/', slash_pos + 1U)) {
        ensure_directory_exists(port, file_path.substr(0, slash_pos), access_token);
    }
}

void expect_invalid_storage_path_response(std::uint16_t port, std::string_view request, std::string_view message) {
    const std::string response = test::http::send_single_request(port, request);
    test::expect_contains(response, "HTTP/1.1 400 Bad Request", message);
    test::expect_contains(response, R"("code":"invalid_path")", message);
}

std::string upload_file_single_chunk(std::uint16_t port, std::string_view path, std::string_view content,
                                     std::string_view access_token) {
    ensure_parent_directories(port, path, access_token);
    const std::string upload_id = init_and_upload_single_chunk(port, path, content, access_token);
    const std::string complete_response = complete_upload(port, upload_id, access_token);
    test::expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should return 200");

    return test::http::response_body(complete_response);
}

std::string get_download_url(std::uint16_t port, std::string_view path, std::string_view access_token) {
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string issue_response = test::http::send_single_request(
        port, test::http::build_http_request("POST", std::format("/api/storage/files/{}/download-ticket", path_b64), "",
                                             "application/json", access_token));
    test::expect_contains(issue_response, "HTTP/1.1 200 OK", "issue download ticket should return 200");

    const std::optional<std::string> download_url =
        test::http::extract_api_data_string_field(test::http::response_body(issue_response), "download_url");
    if (!download_url.has_value()) {
        test::fail("issue download ticket should return download_url");
    }
    return *download_url;
}

void test_storage_node_schema_constraints() {
    const database::DatabaseConfig config = test::database::build_test_database_config();
    auto database_pool = test::database::create_database_pool(config);
    nebula::storage::StorageRepository repository(database_pool);
    test::expect_true(repository.check_schema_ready(),
                      "storage repository readiness should require node type constraints");

    test::database::truncate_database_tables(config);

    const std::string sha256(64U, 'b');
    const std::string object_rel_path =
        std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
    {
        pqxx::connection connection(database::build_connection_info(config));
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
        config,
        [](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'symlink', "
                "NULL, NULL, 1000)",
                std::string("/users/1/schema-invalid-type"));
        },
        "unknown node_type should be rejected by schema");
    expect_storage_node_write_rejected(
        config,
        [](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'file', NULL, "
                "5, 1000)",
                std::string("/users/1/schema-file-missing-sha.txt"));
        },
        "file node should reject missing sha");
    expect_storage_node_write_rejected(
        config,
        [&sha256](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'file', $2, "
                "NULL, 1000)",
                std::string("/users/1/schema-file-missing-size.txt"), sha256);
        },
        "file node should reject missing size");
    expect_storage_node_write_rejected(
        config,
        [&sha256](pqxx::work& tx) {
            tx.exec_params(
                "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) VALUES($1, 'directory', "
                "$2, 5, 1000)",
                std::string("/users/1/schema-directory-with-file-fields"), sha256);
        },
        "directory node should reject file fields");
}

void test_storage_requires_access_token() {
    const test::TempDir files_dir("nebula-storage-auth-required-files");
    const test::TempDir secret_dir("nebula-storage-auth-required-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string path_b64 = nebula::common::base64url_encode("/docs/unauthorized.txt");
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_request = test::http::build_http_request("POST", "/api/storage/uploads/init", init_body);
    const std::string init_response = test::http::send_single_request(server.listening_port(), init_request);
    test::expect_contains(init_response, "HTTP/1.1 401 Unauthorized",
                          "storage request without token should return 401");
    test::expect_contains(init_response, R"("code":"token_missing")",
                          "storage request without token should return token_missing");
}

void test_storage_rejects_non_canonical_user_paths() {
    const test::TempDir files_dir("nebula-storage-invalid-path-files");
    const test::TempDir secret_dir("nebula-storage-invalid-path-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::array<std::string_view, 5> invalid_paths = {
        "a/../b", "/a/../b", "/a//b", "/a/./b", "/a/b/",
    };
    for (const std::string_view invalid_path : invalid_paths) {
        const std::string path_b64 = nebula::common::base64url_encode(invalid_path);
        const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
        expect_invalid_storage_path_response(
            server.listening_port(),
            test::http::build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token),
            "upload init should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", path_b64), "",
                                           "application/json", token),
            "create directory should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            test::http::build_http_request("POST", std::format("/api/storage/files/{}/download-ticket", path_b64), "",
                                           "application/json", token),
            "issue download ticket should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            test::http::build_http_request("GET", std::format("/api/storage/tree/{}", path_b64), "", "application/json",
                                           token),
            "tree list should reject non-canonical storage path");

        expect_invalid_storage_path_response(
            server.listening_port(),
            test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", path_b64), "",
                                           "application/json", token),
            "delete node should reject non-canonical storage path");
    }
}

void test_storage_upload_complete_download_flow() {
    const test::TempDir files_dir("nebula-storage-files");
    const test::TempDir secret_dir("nebula-storage-flow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path = "/docs/hello.txt";
    ensure_directory_exists(server.listening_port(), "/docs", token);
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":2}})", path_b64);
    const std::string init_request =
        test::http::build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token);
    const std::string init_response = test::http::send_single_request(server.listening_port(), init_request);
    test::expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");
    const std::optional<std::string> upload_id =
        test::http::extract_api_data_string_field(test::http::response_body(init_response), "upload_id");
    if (!upload_id.has_value()) {
        test::fail("upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_request =
        test::http::build_http_request("PUT", chunk0_path, "hello ", "application/octet-stream", token);
    const std::string chunk0_response = test::http::send_single_request(server.listening_port(), chunk0_request);
    test::expect_contains(chunk0_response, "HTTP/1.1 200 OK", "chunk 0 should return 200");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_request =
        test::http::build_http_request("PUT", chunk1_path, "nebula", "application/octet-stream", token);
    const std::string chunk1_response = test::http::send_single_request(server.listening_port(), chunk1_request);
    test::expect_contains(chunk1_response, "HTTP/1.1 200 OK", "chunk 1 should return 200");

    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", *upload_id);
    const std::string complete_request =
        test::http::build_http_request("POST", complete_path, "", "application/json", token);
    const std::string complete_response = test::http::send_single_request(server.listening_port(), complete_request);
    test::expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should return 200");

    const std::string complete_again_response =
        test::http::send_single_request(server.listening_port(), complete_request);
    test::expect_contains(complete_again_response, "HTTP/1.1 404 Not Found",
                          "completed upload should not complete again");
    test::expect_contains(complete_again_response, R"("code":"upload_not_found")",
                          "completed upload should report upload_not_found on repeat complete");
    test::expect_contains(complete_again_response, R"("message":"upload not found")",
                          "completed upload should report user-facing missing upload message");

    const std::string download_url = get_download_url(server.listening_port(), path, token);
    const std::string download_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", download_url, "", "application/json"));
    test::expect_contains(download_response, "HTTP/1.1 200 OK", "ticket download should return 200");
    test::expect_contains(download_response, "Content-Disposition: attachment;",
                          "ticket download should provide browser attachment header");
    test::expect_contains(download_response, "filename*=UTF-8''hello.txt",
                          "ticket download should expose utf-8 file name in attachment header");
    test::expect_equal(test::http::response_body(download_response), std::string("hello nebula"),
                       "ticket download body should match upload");
}

void test_storage_download_ticket_flow() {
    const test::TempDir files_dir("nebula-storage-download-ticket-files");
    const test::TempDir secret_dir("nebula-storage-download-ticket-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path = "/docs/report 2026.txt";
    const std::string complete_response_body =
        upload_file_single_chunk(server.listening_port(), path, "download-via-ticket", token);
    const std::optional<std::string> sha256 =
        test::http::extract_api_data_string_field(complete_response_body, "sha256");
    if (!sha256.has_value()) {
        test::fail("upload complete should return sha256 for download ticket flow");
    }

    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string issue_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", std::format("/api/storage/files/{}/download-ticket", path_b64), "",
                                       "application/json", token));
    test::expect_contains(issue_response, "HTTP/1.1 200 OK", "issue download ticket should return 200");
    const std::optional<std::string> download_url =
        test::http::extract_api_data_string_field(test::http::response_body(issue_response), "download_url");
    if (!download_url.has_value()) {
        test::fail("issue download ticket should return download_url");
    }
    const std::optional<std::int64_t> expires_at_s =
        test::http::extract_api_data_int64_field(test::http::response_body(issue_response), "expires_at_s");
    if (!expires_at_s.has_value()) {
        test::fail("issue download ticket should return expires_at_s");
    }
    test::expect_true(*expires_at_s > 0, "download ticket expires_at_s should be positive");
    const std::optional<std::string> ticket = extract_download_ticket_token(*download_url);
    if (!ticket.has_value()) {
        test::fail("download ticket url should contain raw ticket token");
    }
    test::expect_true(ticket->size() == nebula::common::kRandomHexToken128Chars,
                      "download ticket token should use fixed random length");
    test::expect_true(nebula::common::is_valid_random_hex_token_128(*ticket),
                      "download ticket token should be lowercase hex");
    const std::optional<StoredDownloadTicketRow> stored_ticket = fetch_download_ticket_row(config.database, *ticket);
    if (!stored_ticket.has_value()) {
        test::fail("issued download ticket should be persisted in database");
    }
    test::expect_true(stored_ticket->user_id == 1, "issued download ticket should be bound to issuing user");
    test::expect_true(stored_ticket->canonical_path == path, "issued download ticket should store canonical path");
    test::expect_true(stored_ticket->expires_at_s == *expires_at_s,
                      "issued download ticket should persist expiry timestamp");

    const std::string download_with_ticket_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", *download_url, "", "application/json"));
    test::expect_contains(download_with_ticket_response, "HTTP/1.1 200 OK", "ticket download should return 200");
    test::expect_contains(download_with_ticket_response, "Content-Disposition: attachment;",
                          "ticket download should provide browser attachment header");
    test::expect_contains(download_with_ticket_response, "filename*=UTF-8''report%202026.txt",
                          "ticket download should preserve utf-8 encoded file name");
    test::expect_contains(download_with_ticket_response, std::format("ETag: \"{}\"", *sha256),
                          "ticket download should quote sha256 in etag header");
    test::expect_equal(test::http::response_body(download_with_ticket_response), std::string("download-via-ticket"),
                       "ticket download body should match uploaded content");

    const std::string invalid_ticket_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", "/api/storage/downloads/not-a-valid-ticket", "", "application/json"));
    test::expect_contains(invalid_ticket_response, "HTTP/1.1 401 Unauthorized",
                          "invalid ticket download should return 401");
    test::expect_contains(invalid_ticket_response, R"("code":"download_ticket_invalid")",
                          "invalid ticket download should use fixed error code");
}

void test_storage_download_ticket_expired() {
    const test::TempDir files_dir("nebula-storage-download-ticket-expired-files");
    const test::TempDir secret_dir("nebula-storage-download-ticket-expired-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");

    const std::string path = "/docs/expired.txt";
    upload_file_single_chunk(server.listening_port(), path, "download-expired", token);

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_download_tickets(ticket, user_id, canonical_path, created_at_s, expires_at_s) "
            "VALUES($1, $2, $3, $4, $5)",
            std::string(nebula::common::kRandomHexToken128Chars, 'a'), std::int64_t{1}, path, std::int64_t{1000},
            std::int64_t{1001});
        tx.commit();
    }

    const std::string expired_ticket_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request(
            "GET", std::format("/api/storage/downloads/{}", std::string(nebula::common::kRandomHexToken128Chars, 'a')),
            "", "application/json"));
    test::expect_contains(expired_ticket_response, "HTTP/1.1 401 Unauthorized",
                          "expired ticket download should return 401");
    test::expect_contains(expired_ticket_response, R"("code":"download_ticket_expired")",
                          "expired ticket download should use fixed error code");
}

void test_storage_download_ticket_tampered() {
    const test::TempDir files_dir("nebula-storage-download-ticket-tampered-files");
    const test::TempDir secret_dir("nebula-storage-download-ticket-tampered-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Carol_1", "password_123");

    const std::string path = "/docs/tampered.txt";
    upload_file_single_chunk(server.listening_port(), path, "download-tampered", token);

    const std::string download_url = get_download_url(server.listening_port(), path, token);
    const std::optional<std::string> ticket = extract_download_ticket_token(download_url);
    if (!ticket.has_value()) {
        test::fail("issued download url should contain ticket token");
    }

    std::string tampered_ticket = *ticket;
    tampered_ticket.back() = tampered_ticket.back() == 'a' ? 'b' : 'a';
    test::expect_true(tampered_ticket != *ticket, "tampered ticket should differ from original");

    const std::string tampered_ticket_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", std::format("/api/storage/downloads/{}", tampered_ticket), "",
                                       "application/json"));
    test::expect_contains(tampered_ticket_response, "HTTP/1.1 401 Unauthorized",
                          "tampered ticket download should return 401");
    test::expect_contains(tampered_ticket_response, R"("code":"download_ticket_invalid")",
                          "tampered ticket download should use fixed error code");
}

void test_storage_download_ticket_too_long_rejected() {
    const test::TempDir files_dir("nebula-storage-download-ticket-too-long-files");
    const test::TempDir secret_dir("nebula-storage-download-ticket-too-long-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Dave_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/docs/too-long.txt", "download-too-long", token);

    const std::string too_long_ticket(nebula::common::kRandomHexToken128Chars + 1U, 'a');
    const std::string too_long_ticket_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", std::format("/api/storage/downloads/{}", too_long_ticket), "",
                                       "application/json"));
    test::expect_contains(too_long_ticket_response, "HTTP/1.1 401 Unauthorized",
                          "too long ticket download should return 401");
    test::expect_contains(too_long_ticket_response, R"("code":"download_ticket_invalid")",
                          "too long ticket download should use fixed error code");
}

void test_storage_upload_rejects_chunk_after_completion() {
    const test::TempDir files_dir("nebula-storage-upload-bound-files");
    const test::TempDir secret_dir("nebula-storage-upload-bound-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path = "/docs/chunk-bound.txt";
    ensure_directory_exists(server.listening_port(), "/docs", token);
    const std::string path_b64 = nebula::common::base64url_encode(path);
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", path_b64);
    const std::string init_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token));
    test::expect_contains(init_response, "HTTP/1.1 200 OK", "upload init should return 200");
    const std::optional<std::string> upload_id =
        test::http::extract_api_data_string_field(test::http::response_body(init_response), "upload_id");
    if (!upload_id.has_value()) {
        test::fail("upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", chunk0_path, "first", "application/octet-stream", token));
    test::expect_contains(chunk0_response, "HTTP/1.1 200 OK", "first chunk should return 200");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", chunk1_path, "-should-be-rejected", "application/octet-stream", token));
    test::expect_contains(chunk1_response, "HTTP/1.1 409 Conflict", "chunk after upload completion should return 409");
    test::expect_contains(chunk1_response, R"("code":"upload_already_complete")",
                          "chunk after upload completion should return upload_already_complete");

    const std::string complete_path = std::format("/api/storage/uploads/{}/complete", *upload_id);
    const std::string complete_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("POST", complete_path, "", "application/json", token));
    test::expect_contains(complete_response, "HTTP/1.1 200 OK", "upload complete should still return 200");

    const std::string download_url = get_download_url(server.listening_port(), path, token);
    const std::string download_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", download_url, "", "application/json"));
    test::expect_contains(download_response, "HTTP/1.1 200 OK", "ticket download should return 200");
    test::expect_equal(test::http::response_body(download_response), std::string("first"),
                       "extra rejected chunk must not be persisted");
}

void test_storage_upload_chunk_failure_restores_db_size() {
    const test::TempDir files_dir("nebula-storage-upload-restore-files");

    nebula::app::AppConfig config;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);
    auto database_pool = test::database::create_database_pool(config.database);
    nebula::storage::StorageRepository repository(database_pool);

    const std::filesystem::path temp_abs_path = config.storage.root_dir / "temp/retry.part";
    std::filesystem::create_directories(temp_abs_path.parent_path());
    {
        std::ofstream stream(temp_abs_path, std::ios::binary | std::ios::trunc);
        test::expect_true(stream.is_open(), "temp upload file should open");
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
    const auto create_result = repository.create_upload_session(session, 1);
    test::expect_true(create_result.has_value(), "upload session should be created");

    const auto failed_append = repository.append_upload_chunk(
        upload_id, 1, 0, config.storage.root_dir,
        [](const std::filesystem::path& path, std::int64_t) {
            std::ofstream stream(path, std::ios::binary | std::ios::app);
            stream << "partial";
            stream.flush();
            return nebula::storage::AppendTempChunkResult{.status = nebula::storage::AppendTempChunkStatus::Failed,
                                                          .bytes_written = 7};
        },
        [](const std::filesystem::path& path, std::int64_t committed_size_bytes) {
            std::error_code ec;
            std::filesystem::resize_file(path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
            return !ec;
        });
    test::expect_true(!failed_append.has_value(), "partial append failure should return internal error");
    test::expect_true(failed_append.error() == nebula::storage::StorageError::InternalError,
                      "partial append failure should return internal error");
    test::expect_equal(nebula::test::read_all(temp_abs_path), std::string(),
                       "failed chunk append should restore file to db committed size");

    const auto retry_append = repository.append_upload_chunk(
        upload_id, 1, 0, config.storage.root_dir,
        [](const std::filesystem::path& path, std::int64_t) {
            std::ofstream stream(path, std::ios::binary | std::ios::app);
            stream << "complete";
            stream.flush();
            return nebula::storage::AppendTempChunkResult{
                .status = stream.good() ? nebula::storage::AppendTempChunkStatus::Appended
                                        : nebula::storage::AppendTempChunkStatus::Failed,
                .bytes_written = 8};
        },
        [](const std::filesystem::path& path, std::int64_t committed_size_bytes) {
            std::error_code ec;
            std::filesystem::resize_file(path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
            return !ec;
        });
    test::expect_true(retry_append.has_value(), "retry append should succeed");
    test::expect_equal(retry_append->next_chunk_index, std::int64_t{1},
                       "retry append should advance next chunk index once");
    test::expect_equal(nebula::test::read_all(temp_abs_path), std::string("complete"),
                       "retry append should not duplicate partially failed bytes");
}

void test_storage_ref_count_delete_and_gc() {
    const test::TempDir files_dir("nebula-storage-ref-count-files");
    const test::TempDir secret_dir("nebula-storage-ref-count-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string complete1 =
        upload_file_single_chunk(server.listening_port(), "/same/a.txt", "same-content", token);
    const std::string complete2 =
        upload_file_single_chunk(server.listening_port(), "/same/b.txt", "same-content", token);

    const std::optional<std::string> sha1 = test::http::extract_api_data_string_field(complete1, "sha256");
    const std::optional<std::string> sha2 = test::http::extract_api_data_string_field(complete2, "sha256");
    if (!sha1.has_value() || !sha2.has_value()) {
        test::fail("complete should return sha256");
    }
    test::expect_equal(*sha1, *sha2, "same content should deduplicate to same sha");

    const std::optional<std::int64_t> ref_count_2 = fetch_object_ref_count(config.database, *sha1);
    if (!ref_count_2.has_value()) {
        test::fail("object should exist after two uploads");
    }
    test::expect_equal(*ref_count_2, std::int64_t{2}, "ref_count should be 2 after two uploads");

    const std::string path_a_b64 = nebula::common::base64url_encode("/same/a.txt");
    const std::string delete_a_request = test::http::build_http_request(
        "DELETE", std::format("/api/storage/nodes/{}", path_a_b64), "", "application/json", token);
    const std::string delete_a_response = test::http::send_single_request(server.listening_port(), delete_a_request);
    test::expect_contains(delete_a_response, "HTTP/1.1 200 OK", "delete a should return 200");

    const std::optional<std::int64_t> ref_count_1 = fetch_object_ref_count(config.database, *sha1);
    if (!ref_count_1.has_value()) {
        test::fail("object should still exist after deleting one path");
    }
    test::expect_equal(*ref_count_1, std::int64_t{1}, "ref_count should be 1 after deleting one path");

    const std::string path_b_b64 = nebula::common::base64url_encode("/same/b.txt");
    const std::string delete_b_request = test::http::build_http_request(
        "DELETE", std::format("/api/storage/nodes/{}", path_b_b64), "", "application/json", token);
    const std::string delete_b_response = test::http::send_single_request(server.listening_port(), delete_b_request);
    test::expect_contains(delete_b_response, "HTTP/1.1 200 OK", "delete b should return 200");

    const std::string gc_request =
        test::http::build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = test::http::send_single_request(server.listening_port(), gc_request);
    test::expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");

    test::expect_equal(fetch_object_count(config.database), std::int64_t{0},
                       "all object rows should be cleaned after delete and gc");
}

void test_storage_gc_rejects_plain_user() {
    const test::TempDir files_dir("nebula-storage-gc-role-files");
    const test::TempDir secret_dir("nebula-storage-gc-role-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    [[maybe_unused]] const std::string owner_token =
        register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");
    const std::string user_token = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");

    const std::string gc_request =
        test::http::build_http_request("POST", "/api/storage/gc", "", "application/json", user_token);
    const std::string gc_response = test::http::send_single_request(server.listening_port(), gc_request);
    test::expect_contains(gc_response, "HTTP/1.1 403 Forbidden", "plain user gc should return 403");
    test::expect_contains(gc_response, R"("code":"forbidden")", "plain user gc should return forbidden code");
}

void test_storage_gc_cleans_file_only_objects() {
    const test::TempDir files_dir("nebula-storage-file-only-orphan-files");
    const test::TempDir secret_dir("nebula-storage-file-only-orphan-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string complete_response_body =
        upload_file_single_chunk(server.listening_port(), "/gc/file-only-orphan.txt", "orphan-content", token);
    const std::optional<std::string> sha256 =
        test::http::extract_api_data_string_field(complete_response_body, "sha256");
    if (!sha256.has_value()) {
        test::fail("upload complete should return sha256");
    }

    const std::filesystem::path object_file_path =
        config.storage.root_dir / "objects" / sha256->substr(0, 2) / sha256->substr(2, 2) / *sha256;
    test::expect_true(std::filesystem::exists(object_file_path), "object file should exist before gc");

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM storage_nodes WHERE path = $1", std::string("/users/1/gc/file-only-orphan.txt"));
        tx.exec_params("DELETE FROM storage_objects WHERE sha256 = $1", *sha256);
        tx.commit();
    }
    test::expect_equal(fetch_object_count(config.database), std::int64_t{0}, "object table should be empty before gc");
    test::expect_true(std::filesystem::exists(object_file_path), "file-only orphan object should exist before gc");
    {
        std::error_code ec;
        const auto stale_time = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(120);
        std::filesystem::last_write_time(object_file_path, stale_time, ec);
        test::expect_true(!ec, "set file-only orphan object mtime should succeed");
    }

    const std::string gc_request =
        test::http::build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = test::http::send_single_request(server.listening_port(), gc_request);
    test::expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");
    test::expect_contains(gc_response, R"("file_only_objects":1)", "gc should report one file-only object");
    test::expect_contains(gc_response, R"("cleaned_file_only_objects":1)", "gc should clean one file-only object");
    test::expect_true(!std::filesystem::exists(object_file_path), "file-only orphan object should be removed by gc");
}

void test_storage_gc_cleans_orphan_temp_files() {
    const test::TempDir files_dir("nebula-storage-temp-orphan-files");
    const test::TempDir secret_dir("nebula-storage-temp-orphan-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::filesystem::path no_db_temp = config.storage.root_dir / "temp/no-db.part";
    const std::filesystem::path deleted_session_temp = config.storage.root_dir / "temp/deleted-session.part";
    const std::filesystem::path active_temp = config.storage.root_dir / "temp/active-session.part";
    test::write_binary_file(no_db_temp, "no-db");
    test::write_binary_file(deleted_session_temp, "deleted-session");
    test::write_binary_file(active_temp, "active");
    make_file_stale(no_db_temp);
    make_file_stale(deleted_session_temp);
    make_file_stale(active_temp);

    const auto deleted_create_result = storage_runtime.storage_repository->create_upload_session(
        nebula::storage::UploadSessionRecord{
            .upload_id = "deleted-temp-session",
            .path = "/users/1/deleted-temp.txt",
            .temp_rel_path = "temp/deleted-session.part",
            .total_chunks = 1,
            .next_chunk_index = 0,
            .temp_size_bytes = 0,
        },
        1);
    test::expect_true(deleted_create_result.has_value(), "deleted temp upload session should be created");
    const auto active_create_result = storage_runtime.storage_repository->create_upload_session(
        nebula::storage::UploadSessionRecord{
            .upload_id = "active-temp-session",
            .path = "/users/1/active-temp.txt",
            .temp_rel_path = "temp/active-session.part",
            .total_chunks = 1,
            .next_chunk_index = 0,
            .temp_size_bytes = 0,
        },
        1);
    test::expect_true(active_create_result.has_value(), "active temp upload session should be created");

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM storage_upload_sessions WHERE upload_id = $1", std::string("deleted-temp-session"));
        tx.commit();
    }

    const std::string gc_request =
        test::http::build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = test::http::send_single_request(server.listening_port(), gc_request);
    test::expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");
    test::expect_contains(gc_response, R"("orphan_temp_files":2)", "gc should report two orphan temp files");
    test::expect_contains(gc_response, R"("cleaned_orphan_temp_files":2)", "gc should clean two orphan temp files");
    test::expect_contains(gc_response, R"("cleaned_temp_files":2)",
                          "gc should include orphan temp files in cleanup total");
    test::expect_true(!std::filesystem::exists(no_db_temp), "temp file without db row should be removed by gc");
    test::expect_true(!std::filesystem::exists(deleted_session_temp),
                      "temp file for deleted db row should be removed by gc");
    test::expect_true(std::filesystem::exists(active_temp), "active temp file should not be removed by gc");
}

void test_storage_gc_cleans_expired_download_tickets() {
    const test::TempDir files_dir("nebula-storage-expired-ticket-gc-files");
    const test::TempDir secret_dir("nebula-storage-expired-ticket-gc-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_download_tickets(ticket, user_id, canonical_path, created_at_s, expires_at_s) "
            "VALUES($1, $2, $3, $4, $5), ($6, $7, $8, $9, $10)",
            std::string(nebula::common::kRandomHexToken128Chars, 'a'), std::int64_t{1},
            std::string("/docs/expired-a.txt"), std::int64_t{1000}, std::int64_t{1001},
            std::string(nebula::common::kRandomHexToken128Chars, 'b'), std::int64_t{1},
            std::string("/docs/expired-b.txt"), std::int64_t{1000}, std::int64_t{1002});
        tx.commit();
    }
    test::expect_equal(fetch_download_ticket_count(config.database), std::int64_t{2},
                       "expired download tickets should exist before gc");

    const std::string gc_request =
        test::http::build_http_request("POST", "/api/storage/gc", "", "application/json", token);
    const std::string gc_response = test::http::send_single_request(server.listening_port(), gc_request);
    test::expect_contains(gc_response, "HTTP/1.1 200 OK", "gc should return 200");
    test::expect_contains(gc_response, R"("expired_download_tickets":2)", "gc should report expired download tickets");
    test::expect_equal(fetch_download_ticket_count(config.database), std::int64_t{0},
                       "expired download tickets should be removed by gc");
}

void test_storage_temp_cleanup_waits_for_pending_session_reference() {
    const test::TempDir files_dir("nebula-storage-temp-cleanup-race-files");

    nebula::app::AppConfig config;
    config.database = test::database::build_test_database_config();
    config.storage.root_dir = files_dir.path() / "files";
    test::database::truncate_database_tables(config.database);
    auto database_pool = test::database::create_database_pool(config.database);
    nebula::storage::StorageRepository repository(database_pool);

    const nebula::storage::StorageRuntimeConfig route_config{
        .root_dir = config.storage.root_dir,
        .temp_dir = config.storage.root_dir / "temp",
        .objects_dir = config.storage.root_dir / "objects",
        .upload_session_ttl = std::chrono::seconds{86400},
        .download_ticket_ttl = std::chrono::seconds{120},
        .max_body_bytes = std::size_t{1024} * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
    nebula::storage::ObjectStore object_store(route_config);

    const std::string temp_rel_path = "temp/pending-session.part";
    const std::filesystem::path temp_abs_path = route_config.root_dir / temp_rel_path;
    test::write_binary_file(temp_abs_path, "seed");

    pqxx::connection blocker_connection(database::build_connection_info(config.database));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock(hashtextextended($1::text, 0))",
                           std::format("storage_temp:{}", temp_rel_path));
    blocker_tx.exec_params(
        "INSERT INTO storage_upload_sessions(upload_id, path, temp_rel_path, total_chunks, next_chunk_index, "
        "temp_size_bytes, created_at_s, updated_at_s) VALUES($1, $2, $3, 1, 0, $4, $5, $5)",
        std::string("pending-temp-session"), std::string("/users/1/pending-temp.txt"), temp_rel_path, std::int64_t{4},
        std::int64_t{1000});

    auto cleanup_future =
        std::async(std::launch::async, [&repository, &object_store, &temp_abs_path, &temp_rel_path]() {
            return repository.cleanup_temp_file(
                temp_rel_path, temp_abs_path,
                [&object_store](const std::filesystem::path& path) { return object_store.delete_temp_path(path); });
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test::expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                      "temp cleanup should wait for pending upload session reference");

    blocker_tx.commit();
    const nebula::storage::CleanupStatus cleanup_status = cleanup_future.get();

    test::expect_true(cleanup_status == nebula::storage::CleanupStatus::Skipped,
                      "temp cleanup should skip after pending session commits");
    test::expect_true(std::filesystem::exists(temp_abs_path),
                      "temp cleanup should keep temp file after pending session commits");
}

void test_storage_unreferenced_cleanup_avoids_resurrection_race() {
    const test::TempDir files_dir("nebula-storage-cleanup-race-files");
    const test::TempDir secret_dir("nebula-storage-cleanup-race-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);
    auto database_pool = test::database::create_database_pool(config.database);
    nebula::storage::StorageRepository repository(database_pool);

    const nebula::storage::StorageRuntimeConfig route_config{
        .root_dir = config.storage.root_dir,
        .temp_dir = config.storage.root_dir / "temp",
        .objects_dir = config.storage.root_dir / "objects",
        .upload_session_ttl = std::chrono::seconds{86400},
        .download_ticket_ttl = std::chrono::seconds{120},
        .max_body_bytes = std::size_t{1024} * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
    nebula::storage::ObjectStore object_store(route_config);
    std::filesystem::create_directories(route_config.objects_dir);

    const std::string sha256(64U, 'a');
    const std::string object_rel_path = std::format("objects/aa/aa/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    std::filesystem::create_directories(object_abs_path.parent_path());
    {
        std::ofstream out(object_abs_path, std::ios::binary);
        out << "seed";
    }
    test::expect_true(std::filesystem::exists(object_abs_path), "object file should exist before cleanup race test");

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 0, $4, $4)",
            sha256, std::int64_t{4}, object_rel_path, std::int64_t{1000});
        tx.commit();
    }

    pqxx::connection blocker_connection(database::build_connection_info(config.database));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)",
                           nebula::storage::kGlobalStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params("UPDATE storage_objects SET ref_count = 1, updated_at_s = 1001 WHERE sha256 = $1", sha256);
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/race.txt"), sha256, std::int64_t{4}, std::int64_t{1001});

    auto cleanup_future = std::async(std::launch::async, [&repository, &object_store, &sha256, &route_config]() {
        repository.cleanup_unreferenced_object(sha256, [&object_store, &route_config](std::string_view rel_path) {
            return object_store.delete_object_path(route_config.root_dir / rel_path);
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test::expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                      "cleanup should block while shared advisory lock holder is active");

    blocker_tx.commit();
    cleanup_future.get();

    test::expect_true(std::filesystem::exists(object_abs_path),
                      "cleanup should not delete resurrected referenced object file");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(config.database, sha256);
    if (!ref_count.has_value()) {
        test::fail("object row should exist after reference resurrection");
    }
    test::expect_equal(*ref_count, std::int64_t{1}, "ref_count should be 1 after resurrection");
}

void test_storage_upload_failure_cleanup_waits_for_pending_reference() {
    const test::TempDir files_dir("nebula-storage-upload-cleanup-race-files");

    nebula::app::AppConfig config;
    config.database = test::database::build_test_database_config();
    config.storage.root_dir = files_dir.path() / "files";
    test::database::truncate_database_tables(config.database);
    auto database_pool = test::database::create_database_pool(config.database);
    nebula::storage::StorageRepository repository(database_pool);

    const nebula::storage::StorageRuntimeConfig route_config{
        .root_dir = config.storage.root_dir,
        .temp_dir = config.storage.root_dir / "temp",
        .objects_dir = config.storage.root_dir / "objects",
        .upload_session_ttl = std::chrono::seconds{86400},
        .download_ticket_ttl = std::chrono::seconds{120},
        .max_body_bytes = std::size_t{1024} * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
    nebula::storage::ObjectStore object_store(route_config);

    const std::string sha256(64U, 'c');
    const std::string object_rel_path = std::format("objects/cc/cc/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    test::write_binary_file(object_abs_path, "seed");

    pqxx::connection blocker_connection(database::build_connection_info(config.database));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)",
                           nebula::storage::kGlobalStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params(
        "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
        "VALUES($1, $2, $3, 1, $4, $4)",
        sha256, std::int64_t{4}, object_rel_path, std::int64_t{1000});
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/pending-reference.txt"), sha256, std::int64_t{4}, std::int64_t{1000});

    auto cleanup_future = std::async(std::launch::async, [&repository, &object_store, &sha256, &object_abs_path]() {
        repository.cleanup_upload_failure_object(
            object_abs_path, "failed-upload", sha256,
            [&object_store](const std::filesystem::path& path) { return object_store.delete_object_path(path); });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test::expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                      "upload failure cleanup should wait for pending object reference");

    blocker_tx.commit();
    cleanup_future.get();

    test::expect_true(std::filesystem::exists(object_abs_path),
                      "upload failure cleanup should keep object file after pending reference commits");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(config.database, sha256);
    if (!ref_count.has_value()) {
        test::fail("pending referenced object row should exist after cleanup");
    }
    test::expect_equal(*ref_count, std::int64_t{1}, "pending referenced object ref_count should stay 1");
}

void test_storage_file_only_object_cleanup_waits_for_pending_reference() {
    const test::TempDir files_dir("nebula-storage-file-only-cleanup-race-files");

    nebula::app::AppConfig config;
    config.database = test::database::build_test_database_config();
    config.storage.root_dir = files_dir.path() / "files";
    test::database::truncate_database_tables(config.database);
    auto database_pool = test::database::create_database_pool(config.database);
    nebula::storage::StorageRepository repository(database_pool);

    const nebula::storage::StorageRuntimeConfig route_config{
        .root_dir = config.storage.root_dir,
        .temp_dir = config.storage.root_dir / "temp",
        .objects_dir = config.storage.root_dir / "objects",
        .upload_session_ttl = std::chrono::seconds{86400},
        .download_ticket_ttl = std::chrono::seconds{120},
        .max_body_bytes = std::size_t{1024} * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
    nebula::storage::ObjectStore object_store(route_config);

    const std::string sha256(64U, 'd');
    const std::string object_rel_path = std::format("objects/dd/dd/{}", sha256);
    const std::filesystem::path object_abs_path = route_config.root_dir / object_rel_path;
    test::write_binary_file(object_abs_path, "seed");

    pqxx::connection blocker_connection(database::build_connection_info(config.database));
    pqxx::work blocker_tx(blocker_connection);
    blocker_tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)",
                           nebula::storage::kGlobalStorageObjectGcAdvisoryLockKey);
    blocker_tx.exec_params(
        "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
        "VALUES($1, $2, $3, 1, $4, $4)",
        sha256, std::int64_t{4}, object_rel_path, std::int64_t{1000});
    blocker_tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, $4)",
                           std::string("/users/1/file-only-pending-reference.txt"), sha256, std::int64_t{4},
                           std::int64_t{1000});

    auto cleanup_future =
        std::async(std::launch::async, [&repository, &object_store, &object_rel_path, &object_abs_path]() {
            return repository.cleanup_file_only_object(
                object_rel_path, object_abs_path,
                [&object_store](const std::filesystem::path& path) { return object_store.delete_object_path(path); });
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test::expect_true(cleanup_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                      "file-only object cleanup should wait for pending object reference");

    blocker_tx.commit();
    const nebula::storage::CleanupStatus cleanup_status = cleanup_future.get();

    test::expect_true(cleanup_status == nebula::storage::CleanupStatus::Skipped,
                      "file-only object cleanup should skip after pending reference commits");
    test::expect_true(std::filesystem::exists(object_abs_path),
                      "file-only object cleanup should keep object file after pending reference commits");
    const std::optional<std::int64_t> ref_count = fetch_object_ref_count(config.database, sha256);
    if (!ref_count.has_value()) {
        test::fail("pending referenced file-only object row should exist after cleanup");
    }
    test::expect_equal(*ref_count, std::int64_t{1}, "pending referenced file-only object ref_count should stay 1");
}

void test_storage_tree_sorting_and_updated_at() {
    const test::TempDir files_dir("nebula-storage-tree-sort-files");
    const test::TempDir secret_dir("nebula-storage-tree-sort-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    ensure_directory_exists(port, "/dir", token);
    ensure_directory_exists(port, "/dir/sub", token);
    upload_file_single_chunk(port, "/dir/b.txt", "b", token);
    upload_file_single_chunk(port, "/dir/a.txt", "a", token);

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1", std::string("/users/1/dir/sub"),
                       std::int64_t{1001});
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1", std::string("/users/1/dir/a.txt"),
                       std::int64_t{1002});
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1", std::string("/users/1/dir/b.txt"),
                       std::int64_t{1003});
        tx.commit();
    }

    const std::string dir_b64 = nebula::common::base64url_encode("/dir");
    const std::string default_sorted_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", std::format("/api/storage/tree/{}", dir_b64), "",
                                             "application/json", token));
    test::expect_contains(default_sorted_response, "HTTP/1.1 200 OK", "default sorted tree should return 200");
    const ParsedTreeItems default_sorted_items =
        parse_tree_items_response(test::http::response_body(default_sorted_response), "default sorted tree");
    test::expect_equal(default_sorted_items.names.size(), std::size_t{3},
                       "default sorted tree should return 3 direct children");
    test::expect_equal(default_sorted_items.names.at(0), std::string("sub"),
                       "default tree should place directory entries before files");
    test::expect_equal(default_sorted_items.names.at(1), std::string("a.txt"),
                       "default tree should sort files by name ascending");
    test::expect_equal(default_sorted_items.names.at(2), std::string("b.txt"), "default tree should place b.txt last");

    const std::string name_sorted_response = test::http::send_single_request(
        port, test::http::build_http_request(
                  "GET", std::format("/api/storage/tree/{}?sort_by=name&sort_direction=asc", dir_b64), "",
                  "application/json", token));
    test::expect_contains(name_sorted_response, "HTTP/1.1 200 OK", "name sorted tree should return 200");
    const ParsedTreeItems name_sorted_items =
        parse_tree_items_response(test::http::response_body(name_sorted_response), "name sorted tree");
    test::expect_equal(name_sorted_items.names.size(), std::size_t{3},
                       "name sorted tree should return 3 direct children");
    test::expect_equal(name_sorted_items.names.at(0), std::string("sub"), "name sort should place directory first");
    test::expect_equal(name_sorted_items.names.at(1), std::string("a.txt"), "name sort should place a.txt second");
    test::expect_equal(name_sorted_items.names.at(2), std::string("b.txt"), "name sort should place b.txt last");
    test::expect_equal(name_sorted_items.updated_at_values.at(0), std::int64_t{1001},
                       "name sorted tree should return sub updated_at");
    test::expect_equal(name_sorted_items.updated_at_values.at(1), std::int64_t{1002},
                       "name sorted tree should return a.txt updated_at");
    test::expect_equal(name_sorted_items.updated_at_values.at(2), std::int64_t{1003},
                       "name sorted tree should return b.txt updated_at");
    test::expect_equal(name_sorted_items.node_types_by_name.at("a.txt"), std::string("file"),
                       "tree should report file node_type");
    test::expect_equal(name_sorted_items.file_types_by_name.at("a.txt"), std::string("text"),
                       "tree should classify text file type");
    test::expect_equal(name_sorted_items.node_types_by_name.at("sub"), std::string("directory"),
                       "tree should report directory node_type");
    test::expect_equal(name_sorted_items.paths_by_name.at("sub"), std::string("/dir/sub"),
                       "tree should return public directory path");
    test::expect_equal(name_sorted_items.file_counts_by_name.at("sub"), std::int64_t{0},
                       "empty directory should report zero nested file count");

    const std::string modified_sorted_response = test::http::send_single_request(
        port, test::http::build_http_request(
                  "GET", std::format("/api/storage/tree/{}?sort_by=updated_at&sort_direction=desc", dir_b64), "",
                  "application/json", token));
    test::expect_contains(modified_sorted_response, "HTTP/1.1 200 OK", "modified sorted tree should return 200");
    const ParsedTreeItems modified_sorted_items =
        parse_tree_items_response(test::http::response_body(modified_sorted_response), "modified sorted tree");
    test::expect_equal(modified_sorted_items.names.size(), std::size_t{3},
                       "modified sorted tree should return 3 direct children");
    test::expect_equal(modified_sorted_items.names.at(0), std::string("sub"),
                       "modified sort should place directory before files");
    test::expect_equal(modified_sorted_items.names.at(1), std::string("b.txt"),
                       "modified sort should place newest file first");
    test::expect_equal(modified_sorted_items.names.at(2), std::string("a.txt"),
                       "modified sort should place second newest file second");
    test::expect_equal(modified_sorted_items.updated_at_values.at(0), std::int64_t{1001},
                       "modified sorted tree should return directory updated_at first");
    test::expect_equal(modified_sorted_items.updated_at_values.at(1), std::int64_t{1003},
                       "modified sorted tree should return newest file updated_at second");
    test::expect_equal(modified_sorted_items.updated_at_values.at(2), std::int64_t{1002},
                       "modified sorted tree should return second newest file updated_at last");

    const std::string invalid_sort_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", std::format("/api/storage/tree/{}?sort_by=modifiedAt", dir_b64), "",
                                             "application/json", token));
    test::expect_contains(invalid_sort_response, "HTTP/1.1 400 Bad Request",
                          "invalid tree sort_by should return bad request");
    test::expect_contains(invalid_sort_response, R"("code":"invalid_request")",
                          "invalid tree sort_by should return invalid_request");

    const std::string duplicate_sort_response = test::http::send_single_request(
        port, test::http::build_http_request(
                  "GET", std::format("/api/storage/tree/{}?sort_by=name&sort_by=updated_at", dir_b64), "",
                  "application/json", token));
    test::expect_contains(duplicate_sort_response, "HTTP/1.1 400 Bad Request",
                          "duplicate tree sort_by should return bad request");
    test::expect_contains(duplicate_sort_response, R"("code":"invalid_request")",
                          "duplicate tree sort_by should return invalid_request");
}

void test_storage_tree_and_directory_delete() {
    const test::TempDir files_dir("nebula-storage-tree-files");
    const test::TempDir secret_dir("nebula-storage-tree-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/dir/a.txt", "a", token);
    upload_file_single_chunk(server.listening_port(), "/dir/sub/b.txt", "b", token);

    const std::string dir_b64 = nebula::common::base64url_encode("/dir");
    const std::string tree_request = test::http::build_http_request("GET", std::format("/api/storage/tree/{}", dir_b64),
                                                                    "", "application/json", token);
    const std::string tree_response = test::http::send_single_request(server.listening_port(), tree_request);
    test::expect_contains(tree_response, "HTTP/1.1 200 OK", "tree list should return 200");
    test::expect_contains(tree_response, R"("name":"a.txt")", "tree should include file item");
    test::expect_contains(tree_response, R"("name":"sub")", "tree should include sub directory");
    test::expect_contains(tree_response, R"("type":"directory")", "tree should include directory type");
    const ParsedTreeItems parsed_tree =
        parse_tree_items_response(test::http::response_body(tree_response), "tree list");
    test::expect_equal(parsed_tree.file_counts_by_name.at("sub"), std::int64_t{1},
                       "tree should report nested file count for directory");
    test::expect_equal(parsed_tree.file_types_by_name.at("a.txt"), std::string("text"),
                       "tree should classify uploaded text file");

    const std::string delete_dir_request = test::http::build_http_request(
        "DELETE", std::format("/api/storage/nodes/{}", dir_b64), "", "application/json", token);
    const std::string delete_dir_response =
        test::http::send_single_request(server.listening_port(), delete_dir_request);
    test::expect_contains(delete_dir_response, "HTTP/1.1 409 Conflict", "delete non-empty directory should return 409");

    const std::string file_a_b64 = nebula::common::base64url_encode("/dir/a.txt");
    const std::string file_b_b64 = nebula::common::base64url_encode("/dir/sub/b.txt");
    const std::string sub_dir_b64 = nebula::common::base64url_encode("/dir/sub");
    const std::string delete_a_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", file_a_b64), "",
                                       "application/json", token));
    const std::string delete_b_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", file_b_b64), "",
                                       "application/json", token));
    test::expect_contains(delete_a_response, "HTTP/1.1 200 OK", "delete file a should return 200");
    test::expect_contains(delete_a_response, R"("type":"file")", "delete file should report file type");
    test::expect_contains(delete_b_response, "HTTP/1.1 200 OK", "delete file b should return 200");
    const std::string delete_sub_dir_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", sub_dir_b64), "",
                                       "application/json", token));
    test::expect_contains(delete_sub_dir_response, "HTTP/1.1 200 OK", "delete empty sub directory should return 200");
    test::expect_contains(delete_sub_dir_response, R"("type":"directory")",
                          "delete empty sub directory should report directory type");

    const std::string delete_dir_empty_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", dir_b64),
                                                                "", "application/json", token));
    test::expect_contains(delete_dir_empty_response, "HTTP/1.1 200 OK", "delete empty directory should return 200");
}

void test_storage_explicit_directory_contracts() {
    const test::TempDir files_dir("nebula-storage-explicit-dir-files");
    const test::TempDir secret_dir("nebula-storage-explicit-dir-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    const std::string empty_dir_b64 = nebula::common::base64url_encode("/empty");
    const std::string create_empty_response = test::http::send_single_request(
        port, test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", empty_dir_b64), "",
                                             "application/json", token));
    test::expect_contains(create_empty_response, "HTTP/1.1 200 OK", "create explicit directory should return 200");
    test::expect_contains(create_empty_response, R"("type":"directory")",
                          "create directory should report directory type");

    const std::string root_b64 = nebula::common::base64url_encode("/");
    const std::string root_tree_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", std::format("/api/storage/tree/{}", root_b64), "",
                                             "application/json", token));
    test::expect_contains(root_tree_response, "HTTP/1.1 200 OK", "root tree should return 200");
    test::expect_contains(root_tree_response, R"("name":"empty")", "root tree should include created directory");

    const std::string empty_tree_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", std::format("/api/storage/tree/{}", empty_dir_b64), "",
                                             "application/json", token));
    test::expect_contains(empty_tree_response, "HTTP/1.1 200 OK", "empty directory tree should return 200");
    test::expect_contains(empty_tree_response, R"("items":[])", "empty directory tree should be empty");

    const std::string create_existing_response = test::http::send_single_request(
        port, test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", empty_dir_b64), "",
                                             "application/json", token));
    test::expect_contains(create_existing_response, "HTTP/1.1 409 Conflict",
                          "creating existing directory should return 409");
    test::expect_contains(create_existing_response, R"("code":"directory_already_exists")",
                          "creating existing directory should report directory_already_exists");

    const std::string missing_child_b64 = nebula::common::base64url_encode("/missing/child");
    const std::string create_missing_child_response = test::http::send_single_request(
        port, test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", missing_child_b64), "",
                                             "application/json", token));
    test::expect_contains(create_missing_child_response, "HTTP/1.1 404 Not Found",
                          "creating child under missing parent should return 404");
    test::expect_contains(create_missing_child_response, R"("code":"parent_not_found")",
                          "creating child under missing parent should report parent_not_found");

    const std::string missing_b64 = nebula::common::base64url_encode("/missing");
    const std::string missing_tree_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", std::format("/api/storage/tree/{}", missing_b64), "",
                                             "application/json", token));
    test::expect_contains(missing_tree_response, "HTTP/1.1 404 Not Found", "missing directory list should return 404");
    test::expect_contains(missing_tree_response, R"("code":"path_not_found")",
                          "missing directory list should report path_not_found");

    const std::string delete_missing_response = test::http::send_single_request(
        port, test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", missing_b64), "",
                                             "application/json", token));
    test::expect_contains(delete_missing_response, "HTTP/1.1 404 Not Found", "delete missing node should return 404");
    test::expect_contains(delete_missing_response, R"("code":"path_not_found")",
                          "delete missing node should report path_not_found");

    const std::string download_dir_response = test::http::send_single_request(
        port,
        test::http::build_http_request("POST", std::format("/api/storage/files/{}/download-ticket", empty_dir_b64), "",
                                       "application/json", token));
    test::expect_contains(download_dir_response, "HTTP/1.1 409 Conflict",
                          "issue download ticket for directory should return 409");
    test::expect_contains(download_dir_response, R"("code":"not_file")",
                          "issue download ticket for directory should report not_file");

    const std::string missing_file_b64 = nebula::common::base64url_encode("/missing/file.txt");
    const std::string missing_file_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", missing_file_b64);
    const std::string missing_file_init_response = test::http::send_single_request(
        port, test::http::build_http_request("POST", "/api/storage/uploads/init", missing_file_init_body,
                                             "application/json", token));
    test::expect_contains(missing_file_init_response, "HTTP/1.1 404 Not Found",
                          "upload init under missing parent should return 404");
    test::expect_contains(missing_file_init_response, R"("code":"parent_not_found")",
                          "upload init under missing parent should report parent_not_found");

    const std::string dir_as_file_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", empty_dir_b64);
    const std::string dir_as_file_init_response = test::http::send_single_request(
        port, test::http::build_http_request("POST", "/api/storage/uploads/init", dir_as_file_init_body,
                                             "application/json", token));
    test::expect_contains(dir_as_file_init_response, "HTTP/1.1 409 Conflict",
                          "upload init over directory should return 409");
    test::expect_contains(dir_as_file_init_response, R"("code":"path_conflict")",
                          "upload init over directory should report path_conflict");
    test::expect_contains(dir_as_file_init_response,
                          R"("message":"storage path conflicts with an existing file or directory")",
                          "upload init over directory should report user-facing path conflict message");
}

void test_storage_rejects_file_directory_path_collisions() {
    const test::TempDir files_dir("nebula-storage-tree-collision-files");
    const test::TempDir secret_dir("nebula-storage-tree-collision-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 4;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    upload_file_single_chunk(port, "/dir/a.txt", "child", token);
    const std::string dir_path_b64 = nebula::common::base64url_encode("/dir");
    const std::string dir_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", dir_path_b64);
    const std::string dir_init_response = test::http::send_single_request(
        port,
        test::http::build_http_request("POST", "/api/storage/uploads/init", dir_init_body, "application/json", token));
    test::expect_contains(dir_init_response, "HTTP/1.1 409 Conflict",
                          "uploading file over directory should return 409 at init");
    test::expect_contains(dir_init_response, R"("code":"path_conflict")",
                          "uploading file over directory should report path_conflict");

    upload_file_single_chunk(port, "/leaf", "file", token);
    const std::string leaf_child_b64 = nebula::common::base64url_encode("/leaf/a.txt");
    const std::string leaf_child_init_body = std::format(R"({{"path_b64":"{}","total_chunks":1}})", leaf_child_b64);
    const std::string leaf_child_init_response = test::http::send_single_request(
        port, test::http::build_http_request("POST", "/api/storage/uploads/init", leaf_child_init_body,
                                             "application/json", token));
    test::expect_contains(leaf_child_init_response, "HTTP/1.1 409 Conflict",
                          "uploading child under file should return 409 at init");
    test::expect_contains(leaf_child_init_response, R"("code":"parent_not_directory")",
                          "uploading child under file should report parent_not_directory");

    const std::string race_file_upload_id = init_and_upload_single_chunk(port, "/race", "file", token);
    auto file_complete_future = std::async(std::launch::async, [port, race_file_upload_id, token]() {
        return complete_upload(port, race_file_upload_id, token);
    });
    auto dir_create_future = std::async(std::launch::async, [port, token]() {
        const std::string race_b64 = nebula::common::base64url_encode("/race");
        return test::http::send_single_request(
            port, test::http::build_http_request("PUT", std::format("/api/storage/directories/{}", race_b64), "",
                                                 "application/json", token));
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
    test::expect_equal(static_cast<std::int64_t>(ok_count), std::int64_t{1},
                       "exactly one concurrent conflicting file or directory create should complete");
    test::expect_equal(static_cast<std::int64_t>(conflict_count), std::int64_t{1},
                       "exactly one concurrent conflicting operation should return conflict");

    pqxx::connection connection(database::build_connection_info(config.database));
    pqxx::read_transaction tx(connection);
    const auto race_node_count =
        tx.exec_params("SELECT COUNT(*) FROM storage_nodes WHERE path = $1", std::string("/users/1/race"))[0][0]
            .as<std::int64_t>(0);
    test::expect_equal(race_node_count, std::int64_t{1},
                       "concurrent file/directory race should leave one tree node at the path");
}

void test_storage_recent_and_usage_endpoints() {
    const test::TempDir files_dir("nebula-storage-recent-usage-files");
    const test::TempDir secret_dir("nebula-storage-recent-usage-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    upload_file_single_chunk(port, "/assets/photo.png", "photo-bytes", token);
    upload_file_single_chunk(port, "/docs/report.pdf", "pdf", token);
    upload_file_single_chunk(port, "/code/main.ts", "console.log('nebula');", token);

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1",
                       std::string("/users/1/assets/photo.png"), std::int64_t{1003});
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1",
                       std::string("/users/1/docs/report.pdf"), std::int64_t{1002});
        tx.exec_params("UPDATE storage_nodes SET updated_at_s = $2 WHERE path = $1",
                       std::string("/users/1/code/main.ts"), std::int64_t{1001});
        tx.commit();
    }

    const std::string recent_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", "/api/storage/recent?limit=2", "", "application/json", token));
    test::expect_contains(recent_response, "HTTP/1.1 200 OK", "recent should return 200");
    test::expect_contains(recent_response, R"("path":"/assets/photo.png")", "recent should expose public path");
    test::expect_contains(recent_response, R"("type":"file")", "recent should expose type");
    test::expect_contains(recent_response, R"("file_type":"image")", "recent should classify image files");
    test::expect_not_contains(recent_response, R"("path":"/code/main.ts")",
                              "recent limit should exclude older items beyond limit");

    const std::string invalid_recent_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", "/api/storage/recent?limit=0", "", "application/json", token));
    test::expect_contains(invalid_recent_response, "HTTP/1.1 400 Bad Request",
                          "invalid recent limit should return bad request");
    test::expect_contains(invalid_recent_response, R"("code":"invalid_request")",
                          "invalid recent limit should return invalid_request");

    const std::string usage_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", "/api/storage/usage", "", "application/json", token));
    test::expect_contains(usage_response, "HTTP/1.1 200 OK", "usage should return 200");
    const ParsedUsageResponse parsed_usage =
        parse_usage_response(test::http::response_body(usage_response), "usage response");
    test::expect_equal(parsed_usage.total_bytes, std::int64_t{21474836480},
                       "usage should expose default total quota bytes");
    test::expect_equal(parsed_usage.used_bytes, std::int64_t{36},
                       "usage should sum file sizes across all stored files");
    test::expect_equal(parsed_usage.available_bytes, std::int64_t{21474836444}, "usage should expose available bytes");
    test::expect_equal(parsed_usage.used_percent, std::int64_t{0}, "usage percent should round against total quota");
    test::expect_equal(parsed_usage.max_chunk_bytes, static_cast<std::int64_t>(8 * 1024 * 1024),
                       "usage should expose configured chunk size limit");
    test::expect_equal(parsed_usage.max_file_bytes, static_cast<std::int64_t>(512LL * 1024 * 1024),
                       "usage should expose configured file size limit");
    test::expect_equal(parsed_usage.breakdown.size(), std::size_t{3},
                       "usage should return one breakdown item per file type");
    test::expect_equal(parsed_usage.breakdown.at(0).file_type, std::string("code"),
                       "usage should sort breakdown by used bytes descending");
    test::expect_equal(parsed_usage.breakdown.at(0).size_bytes, std::int64_t{22},
                       "usage should report code file bytes");
    test::expect_equal(parsed_usage.breakdown.at(0).file_count, std::int64_t{1}, "usage should report code file count");
    test::expect_equal(parsed_usage.breakdown.at(1).file_type, std::string("image"),
                       "usage should include image breakdown");
    test::expect_equal(parsed_usage.breakdown.at(1).size_bytes, std::int64_t{11},
                       "usage should report image file bytes");
    test::expect_equal(parsed_usage.breakdown.at(2).file_type, std::string("pdf"),
                       "usage should include pdf breakdown");
    test::expect_equal(parsed_usage.breakdown.at(2).size_bytes, std::int64_t{3}, "usage should report pdf file bytes");
}

void test_storage_upload_rejects_user_quota_overflow() {
    const test::TempDir files_dir("nebula-storage-quota-files");
    const test::TempDir secret_dir("nebula-storage-quota-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("UPDATE users SET quota_bytes = $2 WHERE user_id = $1::bigint", std::string("1"),
                       std::int64_t{5});
        tx.commit();
    }

    const std::string upload_id = init_and_upload_single_chunk(port, "/quota-file.txt", "123456", token);
    const std::string complete_response = complete_upload(port, upload_id, token);
    test::expect_contains(complete_response, "HTTP/1.1 413 Content Too Large",
                          "upload complete above quota should return 413");
    test::expect_contains(complete_response, R"("code":"storage_quota_exceeded")",
                          "upload complete above quota should report storage_quota_exceeded");

    const std::string usage_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", "/api/storage/usage", "", "application/json", token));
    test::expect_contains(usage_response, "HTTP/1.1 200 OK", "usage after quota rejection should still return 200");
    const ParsedUsageResponse parsed_usage =
        parse_usage_response(test::http::response_body(usage_response), "usage after quota rejection");
    test::expect_equal(parsed_usage.total_bytes, std::int64_t{5}, "usage should reflect overridden user quota");
    test::expect_equal(parsed_usage.used_bytes, std::int64_t{0}, "quota-rejected upload should not consume user quota");
    test::expect_equal(parsed_usage.max_chunk_bytes, static_cast<std::int64_t>(8 * 1024 * 1024),
                       "usage should keep configured chunk size limit after quota rejection");
    test::expect_equal(parsed_usage.max_file_bytes, static_cast<std::int64_t>(512LL * 1024 * 1024),
                       "usage should keep configured file size limit after quota rejection");
}

void test_storage_usage_clamps_int64_overflow() {
    const test::TempDir files_dir("nebula-storage-usage-overflow-files");
    const test::TempDir secret_dir("nebula-storage-usage-overflow-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::uint16_t port = server.listening_port();
    const std::string token = register_user_and_get_token(port, "Alice_1", "password_123");

    const std::int64_t max_i64 = std::numeric_limits<std::int64_t>::max();
    const std::int64_t first_size = max_i64 - 7;
    const std::int64_t second_size = 32;
    const std::string first_sha(64U, 'a');
    const std::string second_sha(64U, 'b');
    const std::string first_object_rel_path =
        std::format("objects/{}/{}/{}", first_sha.substr(0, 2), first_sha.substr(2, 2), first_sha);
    const std::string second_object_rel_path =
        std::format("objects/{}/{}/{}", second_sha.substr(0, 2), second_sha.substr(2, 2), second_sha);
    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params("UPDATE users SET quota_bytes = $2 WHERE user_id = $1::bigint", std::string("1"), max_i64);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 1, 1000, 1000)",
            first_sha, first_size, first_object_rel_path);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 1, 1000, 1000)",
            second_sha, second_size, second_object_rel_path);
        tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, 1000)",
                       std::string("/users/1/overflow-a.bin"), first_sha, first_size);
        tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, 1000)",
                       std::string("/users/1/overflow-b.bin"), second_sha, second_size);
        tx.commit();
    }

    const std::string usage_response = test::http::send_single_request(
        port, test::http::build_http_request("GET", "/api/storage/usage", "", "application/json", token));
    test::expect_contains(usage_response, "HTTP/1.1 200 OK",
                          "usage should stay available when size sum overflows int64");
    const ParsedUsageResponse parsed_usage =
        parse_usage_response(test::http::response_body(usage_response), "usage overflow");
    test::expect_equal(parsed_usage.total_bytes, max_i64, "usage overflow should keep configured total quota");
    test::expect_equal(parsed_usage.used_bytes, max_i64, "usage overflow should clamp used bytes to int64 max");
    test::expect_equal(parsed_usage.available_bytes, std::int64_t{0},
                       "usage overflow should clamp available bytes to zero when used reaches quota");
    test::expect_equal(parsed_usage.used_percent, std::int64_t{100},
                       "usage overflow should keep a sane rounded usage percent");
    test::expect_equal(parsed_usage.breakdown.size(), std::size_t{1},
                       "usage overflow should still aggregate same file type into one breakdown item");
    test::expect_equal(parsed_usage.breakdown.at(0).size_bytes, max_i64,
                       "usage overflow should clamp breakdown bytes to int64 max");
    test::expect_equal(parsed_usage.breakdown.at(0).file_count, std::int64_t{2},
                       "usage overflow should preserve file count");
}

void test_storage_rejects_files_above_size_limit() {
    const test::TempDir files_dir("nebula-storage-size-limit-files");
    const test::TempDir secret_dir("nebula-storage-size-limit-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.storage.max_file_bytes = 5;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    const std::string path_b64 = nebula::common::base64url_encode("/limited.bin");
    const std::string init_body = std::format(R"({{"path_b64":"{}","total_chunks":2}})", path_b64);
    const std::string init_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", "/api/storage/uploads/init", init_body, "application/json", token));
    test::expect_contains(init_response, "HTTP/1.1 200 OK", "limited upload init should return 200");
    const std::optional<std::string> upload_id =
        test::http::extract_api_data_string_field(test::http::response_body(init_response), "upload_id");
    if (!upload_id.has_value()) {
        test::fail("limited upload init should return upload_id");
    }

    const std::string chunk0_path = std::format("/api/storage/uploads/{}/chunks/0", *upload_id);
    const std::string chunk0_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", chunk0_path, "abc", "application/octet-stream", token));
    test::expect_contains(chunk0_response, "HTTP/1.1 200 OK", "first limited chunk should fit");

    const std::string chunk1_path = std::format("/api/storage/uploads/{}/chunks/1", *upload_id);
    const std::string chunk1_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("PUT", chunk1_path, "def", "application/octet-stream", token));
    test::expect_contains(chunk1_response, "HTTP/1.1 413 Content Too Large",
                          "oversized accumulated file should return 413");
    test::expect_contains(chunk1_response, R"("code":"file_too_large")",
                          "oversized accumulated file should use fixed code");

    const std::string complete_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", std::format("/api/storage/uploads/{}/complete", *upload_id), "",
                                       "application/json", token));
    test::expect_contains(complete_response, "HTTP/1.1 409 Conflict",
                          "rejected oversized chunk should not advance upload");

    const std::string sha256(64U, 'a');
    const std::string object_rel_path =
        std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
    const std::filesystem::path object_abs_path = config.storage.root_dir / object_rel_path;
    std::filesystem::create_directories(object_abs_path.parent_path());
    {
        std::ofstream object_file(object_abs_path, std::ios::binary);
        object_file << "123456";
    }
    {
        pqxx::connection connection(database::build_connection_info(config.database));
        pqxx::work tx(connection);
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, updated_at_s) "
            "VALUES($1, $2, $3, 1, 1000, 1000)",
            sha256, std::int64_t{5}, object_rel_path);
        tx.exec_params("INSERT INTO storage_nodes(path, sha256, size_bytes, updated_at_s) VALUES($1, $2, $3, 1000)",
                       std::string("/users/1/manual-too-large.bin"), sha256, std::int64_t{5});
        tx.commit();
    }

    const std::string download_url = get_download_url(server.listening_port(), "/manual-too-large.bin", token);
    const std::string download_response = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", download_url, "", "application/json"));
    test::expect_contains(download_response, "HTTP/1.1 413 Content Too Large",
                          "ticket download should reject object file above size limit");
    test::expect_contains(download_response, R"("code":"file_too_large")",
                          "ticket download size limit should use fixed code");

    const std::string usage_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", "/api/storage/usage", "", "application/json", token));
    test::expect_contains(usage_response, "HTTP/1.1 200 OK", "usage should return 200 with custom file size limit");
    const ParsedUsageResponse parsed_usage =
        parse_usage_response(test::http::response_body(usage_response), "usage with custom file size limit");
    test::expect_equal(parsed_usage.max_chunk_bytes, static_cast<std::int64_t>(8 * 1024 * 1024),
                       "usage should expose default chunk size limit when not overridden");
    test::expect_equal(parsed_usage.max_file_bytes, std::int64_t{5}, "usage should expose custom max file size limit");
}

void test_storage_prefix_wildcards_match_literals() {
    const test::TempDir files_dir("nebula-storage-prefix-wildcard-files");
    const test::TempDir secret_dir("nebula-storage-prefix-wildcard-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);
    const std::string token = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/literal%/inside.txt", "percent", token);
    upload_file_single_chunk(server.listening_port(), "/literalX/outside.txt", "x", token);
    upload_file_single_chunk(server.listening_port(), "/literal_/inside.txt", "underscore", token);
    upload_file_single_chunk(server.listening_port(), "/literala/outside.txt", "a", token);

    const std::string percent_tree_b64 = nebula::common::base64url_encode("/literal%");
    const std::string percent_tree_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", std::format("/api/storage/tree/{}", percent_tree_b64), "",
                                       "application/json", token));
    test::expect_contains(percent_tree_response, "HTTP/1.1 200 OK", "literal percent tree query should succeed");
    test::expect_contains(percent_tree_response, R"("name":"inside.txt")",
                          "literal percent tree should return literal child");
    test::expect_not_contains(percent_tree_response, R"("outside.txt")",
                              "literal percent tree should not match wildcard siblings");

    const std::string underscore_tree_b64 = nebula::common::base64url_encode("/literal_");
    const std::string underscore_tree_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("GET", std::format("/api/storage/tree/{}", underscore_tree_b64), "",
                                       "application/json", token));
    test::expect_contains(underscore_tree_response, "HTTP/1.1 200 OK", "literal underscore tree query should succeed");
    test::expect_contains(underscore_tree_response, R"("name":"inside.txt")",
                          "literal underscore tree should return literal child");
    test::expect_not_contains(underscore_tree_response, R"("outside.txt")",
                              "literal underscore tree should not match wildcard siblings");

    const std::string percent_file_b64 = nebula::common::base64url_encode("/literal%/inside.txt");
    const std::string percent_dir_b64 = nebula::common::base64url_encode("/literal%");
    const std::string delete_percent_file_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", percent_file_b64), "",
                                       "application/json", token));
    test::expect_contains(delete_percent_file_response, "HTTP/1.1 200 OK",
                          "delete literal percent file should return 200");

    const std::string delete_percent_dir_response = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("DELETE", std::format("/api/storage/nodes/{}", percent_dir_b64), "",
                                       "application/json", token));
    test::expect_contains(delete_percent_dir_response, "HTTP/1.1 200 OK",
                          "empty literal percent dir should ignore wildcard siblings");
}

void test_storage_isolated_between_users() {
    const test::TempDir files_dir("nebula-storage-isolation-files");
    const test::TempDir secret_dir("nebula-storage-isolation-secret");
    const std::filesystem::path secret_path = secret_dir.path() / "jwt.key";
    test::write_jwt_secret_file(secret_path, test::integration::kIntegrationJwtSecret);

    nebula::app::AppConfig config;
    config.server.port = 0;
    config.server.worker_thread_count = 2;
    config.auth.jwt_secret_path = secret_path;
    config.storage.root_dir = files_dir.path() / "files";
    config.database = test::database::build_test_database_config();
    test::database::truncate_database_tables(config.database);

    auto storage_runtime = build_storage_router(config);
    auto server = test::integration::build_runtime(config, storage_runtime.router, storage_runtime.auth_service);
    test::integration::ServerRunGuard server_guard(server);
    test::integration::wait_until_server_ready(server);

    const std::string token_a = register_user_and_get_token(server.listening_port(), "Alice_1", "password_123");
    const std::string token_b = register_user_and_get_token(server.listening_port(), "Bob_1", "password_123");

    upload_file_single_chunk(server.listening_port(), "/private/a.txt", "hello-from-alice", token_a);

    const std::string download_url_by_a = get_download_url(server.listening_port(), "/private/a.txt", token_a);
    const std::string download_by_a = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", download_url_by_a, "", "application/json"));
    test::expect_contains(download_by_a, "HTTP/1.1 200 OK", "owner ticket download should return 200");

    const std::string path_b64 = nebula::common::base64url_encode("/private/a.txt");
    const std::string issue_ticket_by_b = test::http::send_single_request(
        server.listening_port(),
        test::http::build_http_request("POST", std::format("/api/storage/files/{}/download-ticket", path_b64), "",
                                       "application/json", token_b));
    test::expect_contains(issue_ticket_by_b, "HTTP/1.1 404 Not Found", "other user issue ticket should return 404");

    const std::string root_b64 = nebula::common::base64url_encode("/");
    const std::string tree_by_b = test::http::send_single_request(
        server.listening_port(), test::http::build_http_request("GET", std::format("/api/storage/tree/{}", root_b64),
                                                                "", "application/json", token_b));
    test::expect_contains(tree_by_b, "HTTP/1.1 200 OK", "other user tree list should return 200");
    test::expect_not_contains(tree_by_b, R"("private")", "other user tree should not include first user folder");
}

int run_http_server_storage_db_integration_tests() {
    nebula::common::Logger::instance().set_level(nebula::common::LogLevel::Warning);
    test::database::require_database_test_env();

    const std::vector<nebula::test::TestCase> tests = {
        {"storage node schema constraints", test_storage_node_schema_constraints},
        {"storage requires access token", test_storage_requires_access_token},
        {"storage rejects non canonical user paths", test_storage_rejects_non_canonical_user_paths},
        {"storage upload complete download flow", test_storage_upload_complete_download_flow},
        {"storage download ticket flow", test_storage_download_ticket_flow},
        {"storage download ticket expired", test_storage_download_ticket_expired},
        {"storage download ticket tampered", test_storage_download_ticket_tampered},
        {"storage download ticket too long rejected", test_storage_download_ticket_too_long_rejected},
        {"storage upload rejects chunk after completion", test_storage_upload_rejects_chunk_after_completion},
        {"storage upload chunk failure restores db size", test_storage_upload_chunk_failure_restores_db_size},
        {"storage ref count delete and gc", test_storage_ref_count_delete_and_gc},
        {"storage gc rejects plain user", test_storage_gc_rejects_plain_user},
        {"storage gc cleans file-only objects", test_storage_gc_cleans_file_only_objects},
        {"storage gc cleans orphan temp files", test_storage_gc_cleans_orphan_temp_files},
        {"storage gc cleans expired download tickets", test_storage_gc_cleans_expired_download_tickets},
        {"storage temp cleanup waits for pending session reference",
         test_storage_temp_cleanup_waits_for_pending_session_reference},
        {"storage unreferenced cleanup avoids resurrection race",
         test_storage_unreferenced_cleanup_avoids_resurrection_race},
        {"storage upload failure cleanup waits for pending reference",
         test_storage_upload_failure_cleanup_waits_for_pending_reference},
        {"storage file-only object cleanup waits for pending reference",
         test_storage_file_only_object_cleanup_waits_for_pending_reference},
        {"storage tree sorting and updated_at", test_storage_tree_sorting_and_updated_at},
        {"storage tree and directory delete", test_storage_tree_and_directory_delete},
        {"storage explicit directory contracts", test_storage_explicit_directory_contracts},
        {"storage rejects file directory path collisions", test_storage_rejects_file_directory_path_collisions},
        {"storage recent and usage endpoints", test_storage_recent_and_usage_endpoints},
        {"storage upload rejects user quota overflow", test_storage_upload_rejects_user_quota_overflow},
        {"storage usage clamps int64 overflow", test_storage_usage_clamps_int64_overflow},
        {"storage rejects files above size limit", test_storage_rejects_files_above_size_limit},
        {"storage prefix wildcards match literals", test_storage_prefix_wildcards_match_literals},
        {"storage isolated between users", test_storage_isolated_between_users},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_http_server_storage_db_integration_tests);
}
