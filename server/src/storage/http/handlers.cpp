#include "nebula/storage/http/handlers.hpp"

#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "nebula/auth/domain/user.hpp"
#include "nebula/common/base/string.hpp"
#include "nebula/common/codec/json.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/http/codec/response_writer.hpp"
#include "nebula/http/redaction/request_redaction.hpp"
#include "nebula/storage/domain/file_types.hpp"
#include "nebula/storage/domain/path.hpp"
#include "nebula/storage/domain/permissions.hpp"
#include "nebula/storage/domain/types.hpp"
#include "nebula/storage/http/responses.hpp"

namespace nebula::storage {

namespace {

constexpr std::int64_t kDefaultRecentLimit = 8;
constexpr std::int64_t kMaxRecentLimit = 64;

struct StorageRequestUserContext {
    bool ok = false;
    std::int64_t user_id = 0;
    auth::UserRole role = auth::UserRole::User;
    http::HttpResponse error_response;
};

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

std::optional<DirectoryListSortBy> parse_directory_list_sort_by(std::string_view value) {
    if (value == "name") {
        return DirectoryListSortBy::Name;
    }
    if (value == "updated_at") {
        return DirectoryListSortBy::UpdatedAt;
    }
    return std::nullopt;
}

std::optional<DirectoryListSortDirection> parse_directory_list_sort_direction(std::string_view value) {
    if (value == "asc") {
        return DirectoryListSortDirection::Asc;
    }
    if (value == "desc") {
        return DirectoryListSortDirection::Desc;
    }
    return std::nullopt;
}

std::optional<DirectoryListOptions> parse_directory_list_options(const http::QueryParams& query_params) {
    DirectoryListOptions options;

    const auto sort_by_it = query_params.find("sort_by");
    if (sort_by_it != query_params.end()) {
        if (sort_by_it->second.size() != 1U) {
            return std::nullopt;
        }
        const std::optional<DirectoryListSortBy> parsed_sort_by =
            parse_directory_list_sort_by(sort_by_it->second.front());
        if (!parsed_sort_by.has_value()) {
            return std::nullopt;
        }
        options.sort_by = *parsed_sort_by;
    }

    const auto sort_direction_it = query_params.find("sort_direction");
    if (sort_direction_it != query_params.end()) {
        if (sort_direction_it->second.size() != 1U) {
            return std::nullopt;
        }
        const std::optional<DirectoryListSortDirection> parsed_sort_direction =
            parse_directory_list_sort_direction(sort_direction_it->second.front());
        if (!parsed_sort_direction.has_value()) {
            return std::nullopt;
        }
        options.sort_direction = *parsed_sort_direction;
    }

    return options;
}

std::optional<std::int64_t> parse_recent_limit(const http::QueryParams& query_params) {
    const auto limit_it = query_params.find("limit");
    if (limit_it == query_params.end()) {
        return kDefaultRecentLimit;
    }
    if (limit_it->second.size() != 1U) {
        return std::nullopt;
    }

    const auto limit = common::parse_number<std::int64_t>(limit_it->second.front());
    if (!limit.has_value() || *limit <= 0 || *limit > kMaxRecentLimit) {
        return std::nullopt;
    }
    return *limit;
}

std::optional<UploadInitRequest> parse_upload_init_request(std::string_view body) {
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

std::string file_name_from_path(std::string_view path) {
    const std::size_t slash_pos = path.rfind('/');
    if (slash_pos == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(slash_pos + 1U));
}

bool is_attr_char(unsigned char ch) {
    static constexpr std::string_view kAttrCharPunct = "!#$&+-.^_`|~";
    return std::isalnum(ch) != 0 || kAttrCharPunct.find(static_cast<char>(ch)) != std::string_view::npos;
}

std::string encode_http_ext_value(std::string_view value) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3U);
    for (const unsigned char ch : value) {
        if (is_attr_char(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4U) & 0x0FU]);
        encoded.push_back(kHex[ch & 0x0FU]);
    }
    return encoded;
}

std::string sanitize_ascii_filename(std::string_view file_name) {
    std::string sanitized;
    sanitized.reserve(file_name.size());
    for (const unsigned char ch : file_name) {
        if (ch < 0x20U || ch == 0x7FU || ch == '"' || ch == '\\' || ch == '/' || ch == ';') {
            sanitized.push_back('_');
            continue;
        }
        if (ch < 0x80U) {
            sanitized.push_back(static_cast<char>(ch));
            continue;
        }
        sanitized.push_back('_');
    }
    return sanitized;
}

std::string build_attachment_content_disposition(std::string_view canonical_path) {
    std::string file_name = file_name_from_path(canonical_path);
    if (file_name.empty()) {
        file_name = "download.bin";
    }
    const std::string ascii_file_name = sanitize_ascii_filename(file_name);
    const std::string encoded_file_name = encode_http_ext_value(file_name);
    return std::format("attachment; filename=\"{}\"; filename*=UTF-8''{}", ascii_file_name, encoded_file_name);
}

StorageRequestUserContext resolve_storage_user_context(const http::RouteContext& context) {
    if (!context.user.has_value()) {
        common::Logger::instance()
            .error("authenticated route missing user context")
            .field("path", http::redact_request_path(context.request.path))
            .field("error", "missing_authenticated_user")
            .field("decision", "return_internal_error");
        return {.ok = false,
                .user_id = 0,
                .role = auth::UserRole::User,
                .error_response = http::make_api_error_response(http::HttpStatus::InternalServerError)};
    }
    return {.ok = true, .user_id = context.user->user_id, .role = context.user->role, .error_response = {}};
}

}  // namespace

http::HttpResponse handle_create_directory(const std::shared_ptr<StorageService>& service,
                                           const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const auto canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const auto result = service->create_directory(user.user_id, *canonical_path);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("path", result->path);
    data.emplace("type", "directory");
    data.emplace("result", "created");
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_upload_init(const std::shared_ptr<StorageService>& service,
                                      const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const std::optional<UploadInitRequest> request = parse_upload_init_request(context.request.body);
    if (!request.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const auto canonical_path = decode_and_validate_path(request->path_b64);
    if (!canonical_path.has_value() || *canonical_path == "/") {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const auto result = service->init_upload(
        user.user_id, InitUploadCmd{.canonical_path = *canonical_path, .total_chunks = request->total_chunks});
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("upload_id", result->upload_id);
    data.emplace("path", result->path);
    data.emplace("total_chunks", result->total_chunks);
    data.emplace("next_chunk_index", result->next_chunk_index);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_upload_chunk(const std::shared_ptr<StorageService>& service,
                                       const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto upload_id_it = context.params.find("upload_id");
    const auto chunk_idx_it = context.params.find("chunk_index");
    if (upload_id_it == context.params.end() || chunk_idx_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }

    const auto chunk_index = common::parse_number<std::int64_t>(chunk_idx_it->second);
    if (!chunk_index.has_value() || *chunk_index < 0) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_chunk_index",
                                             "invalid chunk index");
    }

    const auto result = service->append_chunk(user.user_id, upload_id_it->second, *chunk_index, context.request.body);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("upload_id", result->upload_id);
    data.emplace("chunk_index", result->chunk_index);
    data.emplace("total_chunks", result->total_chunks);
    data.emplace("next_chunk_index", result->next_chunk_index);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_upload_complete(const std::shared_ptr<StorageService>& service,
                                          const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto upload_id_it = context.params.find("upload_id");
    if (upload_id_it == context.params.end() || upload_id_it->second.empty()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }

    const auto result = service->complete_upload(user.user_id, upload_id_it->second);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("upload_id", result->upload_id);
    data.emplace("path", result->path);
    data.emplace("sha256", result->sha256);
    data.emplace("size_bytes", result->size_bytes);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_issue_download_ticket(const std::shared_ptr<StorageService>& service,
                                                const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const auto canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value() || *canonical_path == "/") {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const auto result = service->issue_download_ticket(user.user_id, *canonical_path);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("path", result->path);
    data.emplace("download_url", std::format("/api/storage/downloads/{}", result->ticket));
    data.emplace("expires_at_s", result->expires_at_s);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_download_with_ticket(const std::shared_ptr<StorageService>& service,
                                               const http::RouteContext& context) {
    const auto ticket_it = context.params.find("download_ticket");
    if (ticket_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }

    const auto result = service->prepare_download(ticket_it->second);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    http::HttpResponse response;
    response.status = http::HttpStatus::OK;
    response.headers.emplace("Content-Type", "application/octet-stream");
    response.headers.emplace("Content-Disposition", build_attachment_content_disposition(result->canonical_path));
    response.headers.emplace("Cache-Control", "private, no-store");
    response.headers.emplace("X-Content-Type-Options", "nosniff");
    response.headers.emplace("ETag", std::format("\"{}\"", result->sha256));
    response.body = result->body;
    return response;
}

http::HttpResponse handle_tree_list(const std::shared_ptr<StorageService>& service, const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const auto canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const std::optional<DirectoryListOptions> options = parse_directory_list_options(context.request.query_params);
    if (!options.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request",
                                             "invalid tree query params");
    }

    const auto result = service->list_directory(user.user_id, *canonical_path, *options);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonArray item_array;
    item_array.reserve(result->items.size());
    for (const TreeItem& item : result->items) {
        common::JsonObject object;
        object.emplace("name", item.name);
        object.emplace("path", item.path);
        object.emplace("type", to_string(item.node_type));
        object.emplace("updated_at", item.updated_at_s);
        if (item.node_type == StorageNodeType::File) {
            object.emplace("size_bytes", item.size_bytes);
            object.emplace("file_type", classify_file_type(item.path));
        } else {
            object.emplace("file_count", item.file_count);
        }
        item_array.emplace_back(std::move(object));
    }

    common::JsonObject data;
    data.emplace("path", result->path);
    data.emplace("items", std::move(item_array));
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_recent(const std::shared_ptr<StorageService>& service, const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const std::optional<std::int64_t> limit = parse_recent_limit(context.request.query_params);
    if (!limit.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request",
                                             "invalid recent query params");
    }

    const auto result = service->list_recent(user.user_id, *limit);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonArray item_array;
    item_array.reserve(result->items.size());
    for (const RecentFileItem& item : result->items) {
        common::JsonObject object;
        object.emplace("name", file_name_from_path(item.path));
        object.emplace("path", item.path);
        object.emplace("type", "file");
        object.emplace("file_type", classify_file_type(item.path));
        object.emplace("size_bytes", item.size_bytes);
        object.emplace("updated_at", item.updated_at_s);
        item_array.emplace_back(std::move(object));
    }

    common::JsonObject data;
    data.emplace("limit", result->limit);
    data.emplace("items", std::move(item_array));
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_usage(const std::shared_ptr<StorageService>& service, const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto result = service->usage(user.user_id);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonArray breakdown_array;
    breakdown_array.reserve(result->breakdown.size());
    for (const UsageBreakdownItem& item : result->breakdown) {
        common::JsonObject object;
        object.emplace("file_type", item.file_type);
        object.emplace("size_bytes", item.size_bytes);
        object.emplace("file_count", item.file_count);
        breakdown_array.emplace_back(std::move(object));
    }

    common::JsonObject data;
    data.emplace("total_bytes", result->total_bytes);
    data.emplace("used_bytes", result->used_bytes);
    data.emplace("available_bytes", result->available_bytes);
    data.emplace("used_percent", result->used_percent);
    data.emplace("max_chunk_bytes", result->max_chunk_bytes);
    data.emplace("max_file_bytes", result->max_file_bytes);
    data.emplace("breakdown", std::move(breakdown_array));
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_delete_node(const std::shared_ptr<StorageService>& service,
                                      const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }

    const auto path_b64_it = context.params.find("path_b64");
    if (path_b64_it == context.params.end()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request path");
    }
    const auto canonical_path = decode_and_validate_path(path_b64_it->second);
    if (!canonical_path.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
    }

    const auto result = service->delete_node(user.user_id, *canonical_path);
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("path", result->path);
    data.emplace("type", result->node_type == StorageNodeType::Directory ? "directory" : "file");
    if (result->node_type == StorageNodeType::Directory) {
        data.emplace("result", "deleted");
    }
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_gc(const std::shared_ptr<StorageService>& service, const http::RouteContext& context) {
    const StorageRequestUserContext user = resolve_storage_user_context(context);
    if (!user.ok) {
        return user.error_response;
    }
    if (!can_run_global_storage_gc(user.role)) {
        common::Logger::instance()
            .warn("storage gc rejected")
            .field("user_id", user.user_id)
            .field("role", auth::to_string(user.role))
            .field("error", "forbidden")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::Forbidden, "forbidden", "forbidden");
    }

    const auto result = service->run_gc();
    if (!result.has_value()) {
        return to_http_response(result.error());
    }

    common::JsonObject data;
    data.emplace("expired_upload_sessions", result->expired_upload_sessions);
    data.emplace("expired_download_tickets", result->expired_download_tickets);
    data.emplace("cleaned_temp_files", result->cleaned_temp_files);
    data.emplace("unreferenced_objects", result->unreferenced_objects);
    data.emplace("cleaned_unreferenced_objects", result->cleaned_unreferenced_objects);
    data.emplace("file_only_objects", result->file_only_objects);
    data.emplace("cleaned_file_only_objects", result->cleaned_file_only_objects);
    data.emplace("orphan_temp_files", result->orphan_temp_files);
    data.emplace("cleaned_orphan_temp_files", result->cleaned_orphan_temp_files);
    return http::make_api_success_response(std::move(data));
}

}  // namespace nebula::storage
