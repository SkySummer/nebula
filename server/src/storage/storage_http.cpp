#include "nebula/storage/storage_http.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "nebula/common/base64.hpp"
#include "nebula/common/json.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/time_utils.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/http/http_types.hpp"
#include "nebula/http/router.hpp"
#include "nebula/storage/storage_repository.hpp"
#include "nebula/storage/storage_types.hpp"
#include "nebula/storage/storage_utils.hpp"

namespace nebula::storage {

namespace {

constexpr std::int64_t kFileOnlyOrphanMinAgeS = 30;

struct StorageRequestUserContext {
    bool ok = false;
    std::int64_t user_id = 0;
    http::HttpResponse error_response;
};

std::string decode_path_b64(std::string_view path_b64) {
    const std::optional<std::string> decoded = common::base64url_decode_to_string(path_b64);
    if (!decoded.has_value()) {
        return {};
    }
    return *decoded;
}

bool validate_canonical_path(std::string_view path) {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    if (path == "/") {
        return true;
    }
    if (path.back() == '/') {
        return false;
    }

    const std::vector<std::string> segments = http::split_http_path_segments(path);
    if (segments.empty() || !segments.front().empty()) {
        return false;
    }

    for (std::size_t idx = 1; idx < segments.size(); ++idx) {
        const std::string& segment = segments[idx];
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (segment.find('\0') != std::string::npos) {
            return false;
        }
    }
    return true;
}

bool json_get_required_string(const common::JsonObject& object, std::string_view key, std::string& value) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return false;
    }
    const std::string* raw = it->second.get_if_string();
    if (raw == nullptr) {
        return false;
    }
    value = *raw;
    return true;
}

bool json_get_required_int64(const common::JsonObject& object, std::string_view key, std::int64_t& value) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return false;
    }
    const std::int64_t* raw = it->second.get_if_int64();
    if (raw == nullptr) {
        return false;
    }
    value = *raw;
    return true;
}

std::string bytes_to_hex(std::span<const unsigned char> bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out(bytes.size() * 2U, '\0');
    for (std::size_t idx = 0; idx < bytes.size(); ++idx) {
        out[idx * 2U] = kHex[(bytes[idx] >> 4U) & 0x0FU];
        out[(idx * 2U) + 1U] = kHex[bytes[idx] & 0x0FU];
    }
    return out;
}

[[nodiscard]] bool parse_int64_strict(std::string_view text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && ptr == (text.data() + text.size());
}

[[nodiscard]] std::optional<std::string> decode_and_validate_path(std::string_view path_b64) {
    std::string path = decode_path_b64(path_b64);
    if (!validate_canonical_path(path)) {
        return std::nullopt;
    }
    return path;
}

[[nodiscard]] std::optional<UploadInitRequest> parse_upload_init_request(std::string_view body) {
    const common::JsonParseResult parsed = common::parse_json(body);
    if (!parsed.ok) {
        return std::nullopt;
    }
    const common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }

    UploadInitRequest request;
    if (!json_get_required_string(*object, "path_b64", request.path_b64)) {
        return std::nullopt;
    }
    if (!json_get_required_int64(*object, "total_chunks", request.total_chunks)) {
        return std::nullopt;
    }
    if (request.path_b64.empty() || request.total_chunks <= 0) {
        return std::nullopt;
    }
    return request;
}

[[nodiscard]] bool ensure_storage_root_dirs(const StorageRouteConfig& route_config) {
    std::error_code ec;
    std::filesystem::create_directories(route_config.temp_dir, ec);
    if (ec) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "storage dir init failed")
            .field("path", route_config.temp_dir.string())
            .field("errno", ec.value(), ec.message())
            .field("decision", "stop_init");
        return false;
    }
    std::filesystem::create_directories(route_config.objects_dir, ec);
    if (ec) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "storage dir init failed")
            .field("path", route_config.objects_dir.string())
            .field("errno", ec.value(), ec.message())
            .field("decision", "stop_init");
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> generate_upload_id() {
    std::array<unsigned char, 16> random{};
    if (::RAND_priv_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return std::nullopt;
    }
    return bytes_to_hex(std::span<const unsigned char>(random.data(), random.size()));
}

bool restore_binary_file_size(const std::filesystem::path& path, std::int64_t committed_size_bytes) {
    if (committed_size_bytes < 0) {
        return false;
    }

    std::error_code ec;
    const std::uintmax_t current_size = std::filesystem::file_size(path, ec);
    if (ec || std::cmp_less(current_size, committed_size_bytes)) {
        return false;
    }
    if (std::cmp_equal(current_size, committed_size_bytes)) {
        return true;
    }

    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
    return !ec;
}

[[nodiscard]] bool would_exceed_file_size_limit(std::int64_t committed_size_bytes, std::size_t append_size,
                                                std::int64_t max_file_bytes) {
    if (committed_size_bytes < 0 || max_file_bytes <= 0) {
        return true;
    }
    if (committed_size_bytes > max_file_bytes) {
        return true;
    }
    if (append_size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return true;
    }
    const auto append_size_i64 = static_cast<std::int64_t>(append_size);
    return append_size_i64 > (max_file_bytes - committed_size_bytes);
}

[[nodiscard]] UploadChunkFileAppendResult append_binary_file_at_db_size(const std::filesystem::path& path,
                                                                        std::string_view body,
                                                                        std::int64_t committed_size_bytes,
                                                                        std::int64_t max_file_bytes) {
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return {};
    }
    if (would_exceed_file_size_limit(committed_size_bytes, body.size(), max_file_bytes)) {
        return {.status = UploadChunkFileAppendStatus::FileTooLarge, .bytes_written = 0};
    }
    if (!restore_binary_file_size(path, committed_size_bytes)) {
        return {};
    }

    const auto bytes_written = static_cast<std::int64_t>(body.size());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return {};
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.flush();
    const bool write_ok = out.good();
    out.close();

    std::error_code ec;
    const std::uintmax_t actual_size = std::filesystem::file_size(path, ec);
    const std::uintmax_t expected_size =
        static_cast<std::uintmax_t>(committed_size_bytes) + static_cast<std::uintmax_t>(bytes_written);
    if (!write_ok || ec || actual_size != expected_size) {
        restore_binary_file_size(path, committed_size_bytes);
        return {};
    }
    return {.status = UploadChunkFileAppendStatus::Appended, .bytes_written = bytes_written};
}

[[nodiscard]] std::optional<HashAndSize> compute_sha256_for_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    EVP_MD_CTX* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return std::nullopt;
    }
    const bool init_ok = ::EVP_DigestInit_ex(ctx, ::EVP_sha256(), nullptr) == 1;
    if (!init_ok) {
        ::EVP_MD_CTX_free(ctx);
        return std::nullopt;
    }

    std::array<char, static_cast<std::size_t>(64U * 1024U)> buffer{};
    std::int64_t total = 0;
    while (true) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_n = in.gcount();
        if (read_n > 0) {
            if (::EVP_DigestUpdate(ctx, buffer.data(), static_cast<std::size_t>(read_n)) != 1) {
                ::EVP_MD_CTX_free(ctx);
                return std::nullopt;
            }
            total += static_cast<std::int64_t>(read_n);
        }
        if (in.eof()) {
            break;
        }
        if (!in.good()) {
            ::EVP_MD_CTX_free(ctx);
            return std::nullopt;
        }
    }

    unsigned int digest_len = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (::EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
        ::EVP_MD_CTX_free(ctx);
        return std::nullopt;
    }
    ::EVP_MD_CTX_free(ctx);

    HashAndSize result;
    result.sha256_hex = bytes_to_hex(std::span<const unsigned char>(digest.data(), digest_len));
    result.size_bytes = total;
    return result;
}

enum class EnsureObjectFileStatus : std::uint8_t {
    NotAttempted,
    Failed,
    AlreadyExists,
    Created,
};

[[nodiscard]] EnsureObjectFileStatus ensure_object_file(const std::filesystem::path& temp_path,
                                                        const std::filesystem::path& object_path,
                                                        std::string_view expected_sha256,
                                                        std::int64_t expected_size_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(object_path.parent_path(), ec);
    if (ec) {
        return EnsureObjectFileStatus::Failed;
    }
    if (std::filesystem::exists(object_path, ec)) {
        if (ec) {
            return EnsureObjectFileStatus::Failed;
        }
        const std::optional<HashAndSize> existing_hash = compute_sha256_for_file(object_path);
        if (existing_hash.has_value() && existing_hash->sha256_hex == expected_sha256 &&
            existing_hash->size_bytes == expected_size_bytes) {
            return EnsureObjectFileStatus::AlreadyExists;
        }
        common::Logger::instance()
            .error(common::LogDomain::Storage, "object file integrity check failed")
            .field("path", object_path.string())
            .field("error", "sha256_or_size_mismatch")
            .field("decision", "return_internal_error");
        return EnsureObjectFileStatus::Failed;
    }

    const std::optional<std::string> staging_id = generate_upload_id();
    if (!staging_id.has_value()) {
        return EnsureObjectFileStatus::Failed;
    }
    const std::filesystem::path staging_path = std::format("{}.tmp.{}", object_path.string(), *staging_id);

    ec.clear();
    std::filesystem::copy_file(temp_path, staging_path, std::filesystem::copy_options::none, ec);
    if (ec) {
        delete_file_if_exists(staging_path);
        return EnsureObjectFileStatus::Failed;
    }

    const std::optional<HashAndSize> staging_hash = compute_sha256_for_file(staging_path);
    if (!staging_hash.has_value() || staging_hash->sha256_hex != expected_sha256 ||
        staging_hash->size_bytes != expected_size_bytes) {
        delete_file_if_exists(staging_path);
        return EnsureObjectFileStatus::Failed;
    }

    std::filesystem::create_hard_link(staging_path, object_path, ec);
    delete_file_if_exists(staging_path);
    if (!ec) {
        return EnsureObjectFileStatus::Created;
    }

    ec.clear();
    if (std::filesystem::exists(object_path, ec) && !ec) {
        const std::optional<HashAndSize> existing_hash = compute_sha256_for_file(object_path);
        if (existing_hash.has_value() && existing_hash->sha256_hex == expected_sha256 &&
            existing_hash->size_bytes == expected_size_bytes) {
            return EnsureObjectFileStatus::AlreadyExists;
        }
    }
    return EnsureObjectFileStatus::Failed;
}

[[nodiscard]] bool is_regular_file_older_than(const std::filesystem::directory_entry& entry, std::int64_t min_age_s) {
    std::error_code ec;
    const bool is_regular_file = entry.is_regular_file(ec);
    if (ec || !is_regular_file) {
        return false;
    }

    ec.clear();
    const auto last_write_time = entry.last_write_time(ec);
    if (ec) {
        return false;
    }
    const auto file_age_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::filesystem::file_time_type::clock::now() - last_write_time);
    return file_age_s.count() >= min_age_s;
}

std::vector<std::filesystem::path> collect_file_only_objects(
    const StorageRouteConfig& route_config, const std::unordered_set<std::string>& object_rel_paths_in_db,
    std::int64_t min_age_s) {
    std::vector<std::filesystem::path> orphan_files;

    std::error_code ec;
    if (!std::filesystem::exists(route_config.objects_dir, ec) || ec) {
        return orphan_files;
    }

    std::filesystem::recursive_directory_iterator iter(route_config.objects_dir,
                                                       std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iter != end) {
        const std::filesystem::directory_entry entry = *iter;
        if (is_regular_file_older_than(entry, min_age_s)) {
            ec.clear();
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), route_config.root_dir, ec);
            if (!ec) {
                const std::string rel_path_str = rel_path.generic_string();
                if (!object_rel_paths_in_db.contains(rel_path_str)) {
                    orphan_files.push_back(entry.path());
                }
            }
        }

        ec.clear();
        iter.increment(ec);
    }
    return orphan_files;
}

std::vector<std::filesystem::path> collect_orphan_temp_files(
    const StorageRouteConfig& route_config, const std::unordered_set<std::string>& active_temp_rel_paths,
    std::int64_t min_age_s) {
    std::vector<std::filesystem::path> orphan_files;

    std::error_code ec;
    if (!std::filesystem::exists(route_config.temp_dir, ec) || ec) {
        return orphan_files;
    }

    std::filesystem::recursive_directory_iterator iter(route_config.temp_dir,
                                                       std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iter != end) {
        const std::filesystem::directory_entry entry = *iter;
        if (entry.path().extension() == ".part" && is_regular_file_older_than(entry, min_age_s)) {
            ec.clear();
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), route_config.root_dir, ec);
            if (!ec && !active_temp_rel_paths.contains(rel_path.generic_string())) {
                orphan_files.push_back(entry.path());
            }
        }

        ec.clear();
        iter.increment(ec);
    }
    return orphan_files;
}

enum class ReadBinaryFileStatus : std::uint8_t {
    Read,
    TooLarge,
    Failed,
};

[[nodiscard]] ReadBinaryFileStatus read_binary_file(const std::filesystem::path& path, std::int64_t max_file_bytes,
                                                    std::string& out) {
    if (max_file_bytes <= 0) {
        return ReadBinaryFileStatus::TooLarge;
    }

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec || std::cmp_greater(file_size, max_file_bytes) ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return ec ? ReadBinaryFileStatus::Failed : ReadBinaryFileStatus::TooLarge;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return ReadBinaryFileStatus::Failed;
    }

    out.resize(static_cast<std::size_t>(file_size));
    if (!out.empty()) {
        in.read(out.data(), static_cast<std::streamsize>(out.size()));
        if (in.gcount() != static_cast<std::streamsize>(out.size()) || !in.good()) {
            out.clear();
            return ReadBinaryFileStatus::Failed;
        }
    }
    return ReadBinaryFileStatus::Read;
}

StorageRequestUserContext resolve_storage_user_context(const http::RouteContext& context) {
    if (!context.user.has_value()) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "authenticated route missing user context")
            .field("path", context.request.path)
            .field("error", "missing_authenticated_user")
            .field("decision", "return_internal_error");
        return {.ok = false,
                .user_id = 0,
                .error_response = http::make_api_error_response(http::HttpStatus::InternalServerError)};
    }

    return {.ok = true, .user_id = context.user->user_id, .error_response = {}};
}

std::string scoped_storage_path(std::int64_t user_id, std::string_view canonical_path) {
    if (canonical_path == "/") {
        return std::format("/users/{}", user_id);
    }
    return std::format("/users/{}{}", user_id, canonical_path);
}

std::string to_public_storage_path(std::int64_t user_id, std::string_view scoped_path) {
    const std::string prefix = std::format("/users/{}", user_id);
    if (!scoped_path.starts_with(prefix)) {
        return "/";
    }

    std::string public_path(scoped_path.substr(prefix.size()));
    if (public_path.empty()) {
        public_path = "/";
    }
    return public_path;
}

bool register_route(const std::shared_ptr<http::Router>& router, http::HttpMethod method, std::string_view path,
                    http::RouteHandler handler, http::RouteOptions options = {}) {
    if (router->add_route(method, std::string(path), std::move(handler), options)) {
        return true;
    }
    common::Logger::instance()
        .fatal(common::LogDomain::Storage, "register storage route failed")
        .field("method", http::to_string(method))
        .field("path", path)
        .field("error", "register_route_failed")
        .field("decision", "exit_process");
    return false;
}

http::HttpResponse make_parent_not_found_response() {
    return http::make_api_error_response(http::HttpStatus::NotFound, "parent_not_found", "parent directory not found");
}

http::HttpResponse make_parent_not_directory_response() {
    return http::make_api_error_response(http::HttpStatus::Conflict, "parent_not_directory",
                                         "parent path is not a directory");
}

http::HttpResponse make_path_conflict_response() {
    return http::make_api_error_response(http::HttpStatus::Conflict, "path_conflict",
                                         "storage path conflicts with an existing file or directory");
}

http::HttpResponse make_upload_not_found_response() {
    return http::make_api_error_response(http::HttpStatus::NotFound, "upload_not_found", "upload not found");
}

http::HttpResponse handle_create_directory(const std::shared_ptr<StorageRouteConfig>& /*route_config*/,
                                           const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const std::optional<std::string> canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const std::string scoped_path = scoped_storage_path(auth_context.user_id, *canonical_path);
    const CreateDirectoryResult create_result =
        create_directory_node(auth_context.user_id, scoped_path, common::now_epoch_s());
    switch (create_result.status) {
        case CreateDirectoryStatus::Created: {
            common::JsonObject data;
            data.emplace("path", common::JsonValue(*canonical_path));
            data.emplace("type", common::JsonValue("directory"));
            data.emplace("result", common::JsonValue("created"));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case CreateDirectoryStatus::AlreadyExists:
            return http::make_api_error_response(http::HttpStatus::Conflict, "directory_already_exists",
                                                 "directory already exists");
        case CreateDirectoryStatus::ParentNotFound:
            return make_parent_not_found_response();
        case CreateDirectoryStatus::ParentNotDirectory:
            return make_parent_not_directory_response();
        case CreateDirectoryStatus::PathConflict:
            return make_path_conflict_response();
        case CreateDirectoryStatus::InternalError:
            break;
    }

    common::Logger::instance()
        .error(common::LogDomain::Storage, "create directory failed")
        .field("user_id", auth_context.user_id)
        .field("path", scoped_path)
        .field("error", "create_directory_node_failed")
        .field("decision", "return_internal_error");
    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_upload_init(const std::shared_ptr<StorageRouteConfig>& route_config,
                                      const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const std::optional<UploadInitRequest> request = parse_upload_init_request(context.request.body);
    if (!request.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const std::optional<std::string> canonical_path = decode_and_validate_path(request->path_b64);
    if (!canonical_path.has_value() || *canonical_path == "/") {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }
    const std::string scoped_path = scoped_storage_path(auth_context.user_id, *canonical_path);

    const std::optional<std::string> upload_id = generate_upload_id();
    if (!upload_id.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::string temp_rel_path = std::format("temp/{}.part", *upload_id);
    const std::filesystem::path temp_abs_path = route_config->root_dir / temp_rel_path;
    {
        std::ofstream create_temp(temp_abs_path, std::ios::binary | std::ios::trunc);
        if (!create_temp.is_open()) {
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
        }
        create_temp.flush();
        if (!create_temp.good()) {
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
        }
    }

    const UploadSessionRecord session{
        .upload_id = *upload_id,
        .path = scoped_path,
        .temp_rel_path = temp_rel_path,
        .total_chunks = request->total_chunks,
        .next_chunk_index = 0,
    };
    const UploadSessionCreateResult create_result =
        create_upload_session(session, auth_context.user_id, common::now_epoch_s());
    if (create_result.status != UploadSessionCreateStatus::Created) {
        delete_file_if_exists(temp_abs_path);
    }
    switch (create_result.status) {
        case UploadSessionCreateStatus::Created: {
            common::JsonObject data;
            data.emplace("upload_id", common::JsonValue(*upload_id));
            data.emplace("path", common::JsonValue(*canonical_path));
            data.emplace("total_chunks", common::JsonValue(request->total_chunks));
            data.emplace("next_chunk_index", common::JsonValue(static_cast<std::int64_t>(0)));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case UploadSessionCreateStatus::ParentNotFound:
            return make_parent_not_found_response();
        case UploadSessionCreateStatus::ParentNotDirectory:
            return make_parent_not_directory_response();
        case UploadSessionCreateStatus::PathConflict:
            return make_path_conflict_response();
        case UploadSessionCreateStatus::InternalError:
            break;
    }

    common::Logger::instance()
        .error(common::LogDomain::Storage, "upload init failed")
        .field("user_id", auth_context.user_id)
        .field("path", scoped_path)
        .field("error", "create_upload_session_failed")
        .field("decision", "return_internal_error");
    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_upload_chunk(const std::shared_ptr<StorageRouteConfig>& route_config,
                                       const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto upload_id_it = context.params.find("upload_id");
    const auto chunk_idx_it = context.params.find("chunk_index");
    if (upload_id_it == context.params.end() || chunk_idx_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }

    std::int64_t chunk_index = 0;
    if (!parse_int64_strict(chunk_idx_it->second, chunk_index) || chunk_index < 0) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_chunk_index",
                                             "invalid chunk index");
    }

    const UploadChunkAppendResult append_result = append_upload_chunk(
        upload_id_it->second, auth_context.user_id, chunk_index, common::now_epoch_s(), route_config->root_dir,
        [&](const std::filesystem::path& temp_abs_path, std::int64_t committed_size_bytes) {
            return append_binary_file_at_db_size(temp_abs_path, context.request.body, committed_size_bytes,
                                                 route_config->max_file_bytes);
        },
        restore_binary_file_size);

    switch (append_result.status) {
        case UploadChunkAppendStatus::Advanced: {
            common::JsonObject data;
            data.emplace("upload_id", common::JsonValue(upload_id_it->second));
            data.emplace("chunk_index", common::JsonValue(chunk_index));
            data.emplace("total_chunks", common::JsonValue(append_result.total_chunks));
            data.emplace("next_chunk_index", common::JsonValue(append_result.next_chunk_index));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case UploadChunkAppendStatus::NotFound:
            return make_upload_not_found_response();
        case UploadChunkAppendStatus::AlreadyComplete:
            return http::make_api_error_response(http::HttpStatus::Conflict, "invalid_chunk_index",
                                                 "upload already complete");
        case UploadChunkAppendStatus::InvalidChunkIndex:
            return http::make_api_error_response(http::HttpStatus::Conflict, "invalid_chunk_index",
                                                 "unexpected chunk index");
        case UploadChunkAppendStatus::FileTooLarge:
            return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "file_too_large", "file too large");
        case UploadChunkAppendStatus::InternalError:
            break;
    }

    common::Logger::instance()
        .error(common::LogDomain::Storage, "upload chunk failed")
        .field("user_id", auth_context.user_id)
        .field("upload_id", upload_id_it->second)
        .field("chunk_index", chunk_index)
        .field("error", "append_upload_chunk_failed")
        .field("decision", "return_internal_error");
    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_upload_complete(const std::shared_ptr<StorageRouteConfig>& route_config,
                                          const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto upload_id_it = context.params.find("upload_id");
    if (upload_id_it == context.params.end() || upload_id_it->second.empty()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }

    const UploadCompletePrepareResult prepare_result =
        prepare_upload_complete(upload_id_it->second, auth_context.user_id);
    switch (prepare_result.status) {
        case UploadCompletePrepareStatus::Ready:
            break;
        case UploadCompletePrepareStatus::NotFound:
            return make_upload_not_found_response();
        case UploadCompletePrepareStatus::Incomplete:
            return http::make_api_error_response(http::HttpStatus::Conflict, "upload_incomplete",
                                                 "upload is incomplete");
        case UploadCompletePrepareStatus::InternalError:
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::filesystem::path temp_abs_path = route_config->root_dir / prepare_result.temp_rel_path;
    if (prepare_result.temp_size_bytes > route_config->max_file_bytes) {
        return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "file_too_large", "file too large");
    }

    if (!restore_binary_file_size(temp_abs_path, prepare_result.temp_size_bytes)) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<HashAndSize> hash_and_size = compute_sha256_for_file(temp_abs_path);
    if (!hash_and_size.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }
    const std::string sha256 = hash_and_size->sha256_hex;
    const auto size_bytes = hash_and_size->size_bytes;
    if (sha256.size() < 4U) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::string object_rel_path =
        std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
    const std::filesystem::path object_abs_path = route_config->root_dir / object_rel_path;
    EnsureObjectFileStatus object_file_status = EnsureObjectFileStatus::NotAttempted;
    const UploadCompleteFinalizeResult finalize_result = finalize_upload_complete(
        upload_id_it->second, auth_context.user_id, sha256, size_bytes, object_rel_path, common::now_epoch_s(), [&]() {
            object_file_status = ensure_object_file(temp_abs_path, object_abs_path, sha256, size_bytes);
            return object_file_status != EnsureObjectFileStatus::Failed;
        });
    if (finalize_result.status != UploadCompleteFinalizeStatus::Completed) {
        if (object_file_status == EnsureObjectFileStatus::Created) {
            cleanup_upload_failure_object(*route_config, upload_id_it->second, sha256, object_abs_path);
        }
    }
    switch (finalize_result.status) {
        case UploadCompleteFinalizeStatus::Completed:
            break;
        case UploadCompleteFinalizeStatus::NotFound:
            return make_upload_not_found_response();
        case UploadCompleteFinalizeStatus::Incomplete:
            return http::make_api_error_response(http::HttpStatus::Conflict, "upload_incomplete",
                                                 "upload is incomplete");
        case UploadCompleteFinalizeStatus::ParentNotFound:
            return make_parent_not_found_response();
        case UploadCompleteFinalizeStatus::ParentNotDirectory:
            return make_parent_not_directory_response();
        case UploadCompleteFinalizeStatus::PathConflict:
            return make_path_conflict_response();
        case UploadCompleteFinalizeStatus::InternalError:
            common::Logger::instance()
                .error(common::LogDomain::Storage, "upload complete failed")
                .field("user_id", auth_context.user_id)
                .field("upload_id", upload_id_it->second)
                .field("error", "finalize_upload_complete_failed")
                .field("decision", "return_internal_error");
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    delete_file_if_exists(temp_abs_path);
    try_cleanup_empty_parents(temp_abs_path, route_config->temp_dir);

    if (finalize_result.cleanup_old_sha.has_value()) {
        cleanup_unreferenced_object(*route_config, *finalize_result.cleanup_old_sha);
    }

    common::JsonObject data;
    data.emplace("upload_id", common::JsonValue(upload_id_it->second));
    data.emplace("path", common::JsonValue(to_public_storage_path(auth_context.user_id, finalize_result.scoped_path)));
    data.emplace("sha256", common::JsonValue(sha256));
    data.emplace("size_bytes", common::JsonValue(size_bytes));
    return http::make_api_success_response(common::JsonValue(std::move(data)));
}

http::HttpResponse handle_file_download(const std::shared_ptr<StorageRouteConfig>& route_config,
                                        const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const std::optional<std::string> canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value() || *canonical_path == "/") {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }
    const std::string scoped_path = scoped_storage_path(auth_context.user_id, *canonical_path);

    const FileLookupResult lookup = find_file_node(scoped_path);
    switch (lookup.status) {
        case FileLookupStatus::Found:
            break;
        case FileLookupStatus::NotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "path_not_found", "path not found");
        case FileLookupStatus::Directory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "not_file", "path is not a file");
        case FileLookupStatus::InternalError:
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }
    if (lookup.node.size_bytes < 0 || lookup.node.size_bytes > route_config->max_file_bytes) {
        return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "file_too_large", "file too large");
    }

    std::string body;
    const ReadBinaryFileStatus read_status =
        read_binary_file(route_config->root_dir / lookup.node.object_rel_path, route_config->max_file_bytes, body);
    switch (read_status) {
        case ReadBinaryFileStatus::Read: {
            http::HttpResponse response;
            response.status = http::HttpStatus::OK;
            response.headers.emplace("Content-Type", "application/octet-stream");
            response.headers.emplace("ETag", lookup.node.sha256);
            response.body = std::move(body);
            return response;
        }
        case ReadBinaryFileStatus::TooLarge:
            return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "file_too_large", "file too large");
        case ReadBinaryFileStatus::Failed:
            break;
    }

    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_tree_list(const std::shared_ptr<StorageRouteConfig>& /*route_config*/,
                                    const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const std::optional<std::string> canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }
    const std::string scoped_path = scoped_storage_path(auth_context.user_id, *canonical_path);

    const DirectoryListResult list_result =
        list_directory_children(auth_context.user_id, scoped_path, common::now_epoch_s());
    switch (list_result.status) {
        case DirectoryListStatus::Listed: {
            common::JsonArray item_array;
            item_array.reserve(list_result.items.size());
            for (const TreeItem& item : list_result.items) {
                common::JsonObject object;
                object.emplace("name", common::JsonValue(item.name));
                object.emplace("type", common::JsonValue(item.is_directory ? "directory" : "file"));
                if (!item.is_directory) {
                    object.emplace("size_bytes", common::JsonValue(item.size_bytes));
                }
                item_array.emplace_back(std::move(object));
            }

            common::JsonObject data;
            data.emplace("path", common::JsonValue(*canonical_path));
            data.emplace("items", common::JsonValue(std::move(item_array)));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case DirectoryListStatus::NotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "path_not_found", "path not found");
        case DirectoryListStatus::NotDirectory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "not_directory",
                                                 "path is not a directory");
        case DirectoryListStatus::InternalError:
            break;
    }

    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_delete_node(const std::shared_ptr<StorageRouteConfig>& route_config,
                                      const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const std::optional<std::string> canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }
    if (*canonical_path == "/") {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "root_delete_not_allowed",
                                             "cannot delete root");
    }
    const std::string scoped_path = scoped_storage_path(auth_context.user_id, *canonical_path);

    const DeleteNodeResult delete_result = delete_node_record(auth_context.user_id, scoped_path, common::now_epoch_s());
    switch (delete_result.status) {
        case DeleteNodeStatus::FileDeleted: {
            if (delete_result.cleanup_sha.has_value()) {
                cleanup_unreferenced_object(*route_config, *delete_result.cleanup_sha);
            }

            common::JsonObject data;
            data.emplace("path", common::JsonValue(*canonical_path));
            data.emplace("type", common::JsonValue("file"));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case DeleteNodeStatus::DirectoryDeleted: {
            common::JsonObject data;
            data.emplace("path", common::JsonValue(*canonical_path));
            data.emplace("type", common::JsonValue("directory"));
            data.emplace("result", common::JsonValue("deleted"));
            return http::make_api_success_response(common::JsonValue(std::move(data)));
        }
        case DeleteNodeStatus::NotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "path_not_found", "path not found");
        case DeleteNodeStatus::NonEmptyDirectory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "non_empty_directory",
                                                 "directory is not empty");
        case DeleteNodeStatus::InternalError:
            break;
    }

    common::Logger::instance()
        .error(common::LogDomain::Storage, "delete node failed")
        .field("user_id", auth_context.user_id)
        .field("path", scoped_path)
        .field("error", "delete_node_record_failed")
        .field("decision", "return_internal_error");
    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

http::HttpResponse handle_gc(const std::shared_ptr<StorageRouteConfig>& route_config,
                             const http::RouteContext& context) {
    const StorageRequestUserContext auth_context = resolve_storage_user_context(context);
    if (!auth_context.ok) {
        return auth_context.error_response;
    }

    const std::int64_t now_s = common::now_epoch_s();
    const std::int64_t expired_before_s = now_s - route_config->upload_session_ttl_s;
    const StorageGcSnapshot gc_snapshot = collect_storage_gc_snapshot(expired_before_s, now_s);
    if (gc_snapshot.status != StorageGcSnapshotStatus::Ready) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    std::unordered_set<std::string> object_rel_paths_in_db;
    object_rel_paths_in_db.reserve(gc_snapshot.object_rel_paths_in_db.size());
    for (const std::string& object_rel_path : gc_snapshot.object_rel_paths_in_db) {
        object_rel_paths_in_db.insert(object_rel_path);
    }

    std::unordered_set<std::string> active_temp_rel_paths;
    active_temp_rel_paths.reserve(gc_snapshot.active_temp_rel_paths.size());
    for (const std::string& temp_rel_path : gc_snapshot.active_temp_rel_paths) {
        active_temp_rel_paths.insert(temp_rel_path);
    }

    const std::vector<std::filesystem::path> file_only_objects =
        collect_file_only_objects(*route_config, object_rel_paths_in_db, kFileOnlyOrphanMinAgeS);
    std::int64_t cleaned_file_only_object_count = 0;
    for (const std::filesystem::path& file_only_object : file_only_objects) {
        const CleanupStatus cleanup_status = cleanup_file_only_object(*route_config, file_only_object);
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_file_only_object_count;
        }
    }

    const auto orphan_object_count = static_cast<std::int64_t>(gc_snapshot.orphan_objects.size());
    std::int64_t cleaned_object_count = 0;
    for (const GcOrphanObject& orphan : gc_snapshot.orphan_objects) {
        const CleanupStatus cleanup_status = cleanup_unreferenced_object(*route_config, orphan.sha256);
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_object_count;
        }
    }

    std::int64_t cleaned_temp_count = 0;
    for (const GcExpiredUploadSession& session : gc_snapshot.expired_sessions) {
        const std::filesystem::path temp_abs_path = route_config->root_dir / session.temp_rel_path;
        const CleanupStatus cleanup_status = cleanup_temp_file(*route_config, temp_abs_path);
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_temp_count;
        }
    }

    const std::vector<std::filesystem::path> orphan_temp_files =
        collect_orphan_temp_files(*route_config, active_temp_rel_paths, kFileOnlyOrphanMinAgeS);
    std::int64_t cleaned_orphan_temp_count = 0;
    for (const std::filesystem::path& orphan_temp_file : orphan_temp_files) {
        const CleanupStatus cleanup_status = cleanup_temp_file(*route_config, orphan_temp_file);
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_temp_count;
            ++cleaned_orphan_temp_count;
        }
    }

    common::JsonObject data;
    data.emplace("expired_upload_sessions",
                 common::JsonValue(static_cast<std::int64_t>(gc_snapshot.expired_sessions.size())));
    data.emplace("cleaned_temp_files", common::JsonValue(cleaned_temp_count));
    data.emplace("unreferenced_objects", common::JsonValue(orphan_object_count));
    data.emplace("cleaned_unreferenced_objects", common::JsonValue(cleaned_object_count));
    data.emplace("file_only_objects", common::JsonValue(static_cast<std::int64_t>(file_only_objects.size())));
    data.emplace("cleaned_file_only_objects", common::JsonValue(cleaned_file_only_object_count));
    data.emplace("orphan_temp_files", common::JsonValue(static_cast<std::int64_t>(orphan_temp_files.size())));
    data.emplace("cleaned_orphan_temp_files", common::JsonValue(cleaned_orphan_temp_count));
    return http::make_api_success_response(common::JsonValue(std::move(data)));
}

}  // namespace

bool register_storage_routes(const server::ServerConfig& config, const std::shared_ptr<http::Router>& router) {
    if (router == nullptr) {
        return false;
    }

    if (!check_storage_schema_ready()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Storage, "storage schema check failed")
            .field("error", "storage_schema_not_ready")
            .field("decision", "exit_process");
        return false;
    }

    auto route_config = std::make_shared<StorageRouteConfig>();
    route_config->root_dir = config.storage_root_dir;
    route_config->temp_dir = route_config->root_dir / "temp";
    route_config->objects_dir = route_config->root_dir / "objects";
    route_config->upload_session_ttl_s = config.storage_upload_session_ttl_s;
    route_config->max_body_bytes = config.max_body_bytes;
    route_config->max_file_bytes = config.storage_max_file_bytes;

    if (!ensure_storage_root_dirs(*route_config)) {
        return false;
    }

    if (!register_route(router, http::HttpMethod::Post, "/api/storage/uploads/init",
                        std::bind_front(handle_upload_init, route_config), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/storage/directories/{path_b64}",
                        std::bind_front(handle_create_directory, route_config),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/storage/uploads/{upload_id}/chunks/{chunk_index}",
                        std::bind_front(handle_upload_chunk, route_config), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/storage/uploads/{upload_id}/complete",
                        std::bind_front(handle_upload_complete, route_config),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/files/{path_b64}",
                        std::bind_front(handle_file_download, route_config),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/tree/{path_b64}",
                        std::bind_front(handle_tree_list, route_config), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Delete, "/api/storage/nodes/{path_b64}",
                        std::bind_front(handle_delete_node, route_config), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/storage/gc", std::bind_front(handle_gc, route_config),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }

    return true;
}

}  // namespace nebula::storage
