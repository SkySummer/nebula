#include "nebula/storage/storage_repository.hpp"

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nebula/common/database_utils.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula/storage/storage_utils.hpp"

namespace nebula::storage {

namespace {

void lock_storage_object_gc_sync_exclusive(pqxx::work& tx) {
    tx.exec_params("SELECT pg_advisory_xact_lock($1)", kStorageObjectGcAdvisoryLockKey);
}

void lock_storage_object_gc_sync_shared(pqxx::work& tx) {
    tx.exec_params("SELECT pg_advisory_xact_lock_shared($1)", kStorageObjectGcAdvisoryLockKey);
}

void lock_storage_tree_sync(pqxx::work& tx, std::int64_t user_id) {
    tx.exec_params("SELECT pg_advisory_xact_lock(hashtextextended($1::text, 0))",
                   std::format("storage_tree:/users/{}", user_id));
}

void lock_storage_temp_path_sync(pqxx::work& tx, std::string_view temp_rel_path) {
    tx.exec_params("SELECT pg_advisory_xact_lock(hashtextextended($1::text, 0))",
                   std::format("storage_temp:{}", temp_rel_path));
}

[[nodiscard]] std::string user_root_path(std::int64_t user_id) {
    return std::format("/users/{}", user_id);
}

[[nodiscard]] bool is_user_scoped_storage_path(std::int64_t user_id, std::string_view storage_path) {
    const std::string prefix = user_root_path(user_id);
    if (storage_path.size() < prefix.size()) {
        return false;
    }
    if (!storage_path.starts_with(prefix)) {
        return false;
    }
    return storage_path.size() == prefix.size() || storage_path[prefix.size()] == '/';
}

[[nodiscard]] std::string escape_like_literal(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\\' || ch == '%' || ch == '_') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

[[nodiscard]] std::string build_prefix_like_pattern(std::string_view prefix) {
    std::string pattern = escape_like_literal(prefix);
    pattern.push_back('%');
    return pattern;
}

[[nodiscard]] std::string parent_storage_path(std::string_view path) {
    const std::size_t slash_pos = path.rfind('/');
    if (slash_pos == std::string_view::npos || slash_pos == 0U) {
        return {};
    }
    return std::string(path.substr(0, slash_pos));
}

[[nodiscard]] StorageNodeType require_node_type(std::string_view type) {
    const std::optional<StorageNodeType> parsed = parse_storage_node_type(type);
    if (!parsed.has_value()) {
        throw std::runtime_error("storage_node_type_invalid");
    }
    return *parsed;
}

void ensure_user_root_directory_in_tx(pqxx::work& tx, std::int64_t user_id, std::int64_t now_s) {
    tx.exec_params(
        "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) "
        "VALUES($1, 'directory', NULL, NULL, $2) "
        "ON CONFLICT (path) DO NOTHING",
        user_root_path(user_id), now_s);
}

[[nodiscard]] std::optional<StorageNodeType> find_node_type_for_update(pqxx::work& tx, std::string_view path) {
    const pqxx::result rows =
        tx.exec_params("SELECT node_type FROM storage_nodes WHERE path = $1 FOR UPDATE", std::string(path));
    if (rows.empty()) {
        return std::nullopt;
    }
    return require_node_type(rows.front()[0].as<std::string>());
}

enum class ParentDirectoryStatus : std::uint8_t {
    Ready,
    NotFound,
    NotDirectory,
};

[[nodiscard]] ParentDirectoryStatus validate_parent_directory(pqxx::work& tx, std::string_view scoped_path,
                                                              std::int64_t user_id) {
    const std::string parent = parent_storage_path(scoped_path);
    if (parent.empty() || !is_user_scoped_storage_path(user_id, parent)) {
        return ParentDirectoryStatus::NotFound;
    }

    const std::optional<StorageNodeType> parent_type = find_node_type_for_update(tx, parent);
    if (!parent_type.has_value()) {
        return ParentDirectoryStatus::NotFound;
    }
    if (*parent_type != StorageNodeType::Directory) {
        return ParentDirectoryStatus::NotDirectory;
    }
    return ParentDirectoryStatus::Ready;
}

[[nodiscard]] bool has_descendant_nodes(pqxx::work& tx, std::string_view path) {
    const std::string prefix = std::format("{}/", path);
    const pqxx::result rows = tx.exec_params("SELECT 1 FROM storage_nodes WHERE path LIKE $1 ESCAPE '\\' LIMIT 1",
                                             build_prefix_like_pattern(prefix));
    return !rows.empty();
}

void restore_after_append_failure(
    std::string_view upload_id, std::int64_t chunk_index, const std::filesystem::path& temp_abs_path,
    std::int64_t committed_size_bytes,
    const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file) {
    if (restore_chunk_file(temp_abs_path, committed_size_bytes)) {
        return;
    }

    common::Logger::instance()
        .warn(common::LogDomain::Storage, "upload chunk file restore failed")
        .field("upload_id", upload_id)
        .field("chunk_index", chunk_index)
        .field("path", temp_abs_path.string())
        .field("error", "restore_failed")
        .field("decision", "retry_on_next_chunk_upload");
}

UploadCompleteFinalizeResult finalize_upload_complete_in_tx(pqxx::work& tx, std::string_view upload_id,
                                                            std::int64_t user_id, std::string_view sha256,
                                                            std::int64_t size_bytes, std::string_view object_rel_path,
                                                            std::int64_t now_s) {
    std::optional<std::string> cleanup_old_sha;
    const pqxx::result session_rows = tx.exec_params(
        "SELECT path, total_chunks, next_chunk_index, temp_size_bytes "
        "FROM storage_upload_sessions WHERE upload_id = $1 FOR UPDATE",
        std::string(upload_id));
    if (session_rows.empty()) {
        return {.status = UploadCompleteFinalizeStatus::NotFound, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }

    const pqxx::row session_row = session_rows.front();
    const auto path = session_row[0].as<std::string>();
    if (!is_user_scoped_storage_path(user_id, path)) {
        return {.status = UploadCompleteFinalizeStatus::NotFound, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }

    const auto total_chunks = session_row[1].as<std::int64_t>(0);
    const auto next_chunk_index = session_row[2].as<std::int64_t>(0);
    if (next_chunk_index != total_chunks) {
        return {.status = UploadCompleteFinalizeStatus::Incomplete, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }
    const auto temp_size_bytes = session_row[3].as<std::int64_t>(-1);
    if (temp_size_bytes < 0 || size_bytes != temp_size_bytes) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "finalize upload complete failed")
            .field("upload_id", upload_id)
            .field("error", temp_size_bytes < 0 ? "temp_size_missing" : "temp_size_mismatch")
            .field("decision", "return_internal_error");
        return {
            .status = UploadCompleteFinalizeStatus::InternalError, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }

    lock_storage_tree_sync(tx, user_id);
    ensure_user_root_directory_in_tx(tx, user_id, now_s);

    const ParentDirectoryStatus parent_status = validate_parent_directory(tx, path, user_id);
    switch (parent_status) {
        case ParentDirectoryStatus::Ready:
            break;
        case ParentDirectoryStatus::NotFound:
            return {.status = UploadCompleteFinalizeStatus::ParentNotFound,
                    .scoped_path = {},
                    .cleanup_old_sha = std::nullopt};
        case ParentDirectoryStatus::NotDirectory:
            return {.status = UploadCompleteFinalizeStatus::ParentNotDirectory,
                    .scoped_path = {},
                    .cleanup_old_sha = std::nullopt};
    }
    if (has_descendant_nodes(tx, path)) {
        return {
            .status = UploadCompleteFinalizeStatus::PathConflict, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }

    const pqxx::result existing_rows =
        tx.exec_params("SELECT node_type, sha256 FROM storage_nodes WHERE path = $1 FOR UPDATE", path);
    if (existing_rows.empty()) {
        tx.exec_params(
            "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, "
            "updated_at_s) VALUES($1, $2, $3, 1, $4, $4) "
            "ON CONFLICT (sha256) DO UPDATE SET ref_count = storage_objects.ref_count + 1, updated_at_s = $4",
            std::string(sha256), size_bytes, std::string(object_rel_path), now_s);
        tx.exec_params(
            "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) "
            "VALUES($1, 'file', $2, $3, $4)",
            path, std::string(sha256), size_bytes, now_s);
    } else {
        const pqxx::row existing_row = existing_rows.front();
        if (require_node_type(existing_row[0].as<std::string>()) == StorageNodeType::Directory) {
            return {.status = UploadCompleteFinalizeStatus::PathConflict,
                    .scoped_path = {},
                    .cleanup_old_sha = std::nullopt};
        }
        if (existing_row[1].is_null()) {
            throw std::runtime_error("file_node_sha_missing");
        }
        const auto old_sha = existing_row[1].as<std::string>();
        if (old_sha != sha256) {
            tx.exec_params(
                "INSERT INTO storage_objects(sha256, size_bytes, object_rel_path, ref_count, created_at_s, "
                "updated_at_s) VALUES($1, $2, $3, 1, $4, $4) "
                "ON CONFLICT (sha256) DO UPDATE SET ref_count = storage_objects.ref_count + 1, updated_at_s = $4",
                std::string(sha256), size_bytes, std::string(object_rel_path), now_s);
            tx.exec_params(
                "UPDATE storage_nodes SET node_type = 'file', sha256 = $2, size_bytes = $3, updated_at_s = $4 "
                "WHERE path = $1",
                path, std::string(sha256), size_bytes, now_s);
            const pqxx::result old_object_rows = tx.exec_params(
                "UPDATE storage_objects SET ref_count = ref_count - 1, updated_at_s = $2 "
                "WHERE sha256 = $1 RETURNING ref_count",
                old_sha, now_s);
            if (!old_object_rows.empty() && old_object_rows.front()[0].as<std::int64_t>(0) <= 0) {
                cleanup_old_sha = old_sha;
            }
        } else {
            tx.exec_params(
                "UPDATE storage_nodes SET node_type = 'file', size_bytes = $2, updated_at_s = $3 WHERE path = $1", path,
                size_bytes, now_s);
        }
    }

    tx.exec_params("DELETE FROM storage_upload_sessions WHERE upload_id = $1", std::string(upload_id));
    return {.status = UploadCompleteFinalizeStatus::Completed,
            .scoped_path = path,
            .cleanup_old_sha = std::move(cleanup_old_sha)};
}

}  // namespace

bool check_storage_schema_ready() {
    auto& pool = common::PostgresConnectionPool::instance();
    if (!pool.is_initialized()) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "storage schema readiness check failed")
            .field("error", "connection_pool_not_initialized")
            .field("decision", "return_not_ready");
        return false;
    }

    const auto lease = common::acquire_connection_lease("check_storage_schema_ready");
    if (!lease.has_value()) {
        return false;
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::row row = tx.exec1(
            "SELECT to_regclass('public.storage_objects'), "
            "to_regclass('public.storage_nodes'), "
            "to_regclass('public.storage_upload_sessions'), "
            "NULLIF((SELECT COUNT(*) = 6 FROM information_schema.columns "
            "WHERE table_schema = 'public' AND table_name = 'storage_objects' "
            "AND column_name IN ('sha256', 'size_bytes', 'object_rel_path', 'ref_count', "
            "'created_at_s', 'updated_at_s')), FALSE), "
            "NULLIF((SELECT COUNT(*) = 5 FROM information_schema.columns "
            "WHERE table_schema = 'public' AND table_name = 'storage_nodes' "
            "AND column_name IN ('path', 'node_type', 'sha256', 'size_bytes', 'updated_at_s')), FALSE), "
            "NULLIF((SELECT COUNT(*) = 8 FROM information_schema.columns "
            "WHERE table_schema = 'public' AND table_name = 'storage_upload_sessions' "
            "AND column_name IN ('upload_id', 'path', 'temp_rel_path', 'total_chunks', "
            "'next_chunk_index', 'temp_size_bytes', 'created_at_s', 'updated_at_s')), FALSE)");
        const common::PostgresRowCheckStatus row_status = common::check_postgres_row_non_null_fields(row, 6);
        if (row_status == common::PostgresRowCheckStatus::InvalidSize) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "storage schema readiness check failed")
                .field("error", "schema_check_result_invalid")
                .field("decision", "return_not_ready");
            return false;
        }
        if (row_status == common::PostgresRowCheckStatus::NullField) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "storage schema readiness check failed")
                .field("error", "schema_object_missing")
                .field("decision", "return_not_ready");
            return false;
        }
        common::Logger::instance().info(common::LogDomain::Storage, "storage schema ready");
        return true;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "storage schema readiness check failed")
            .field("error", e.what())
            .field("decision", "return_not_ready");
        return false;
    }
}

bool ensure_user_root_directory(std::int64_t user_id, std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("ensure_user_root_directory");
    if (!lease.has_value()) {
        return false;
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s);
        tx.commit();
        return true;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "ensure user root directory failed")
            .field("user_id", user_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return false;
    }
}

FileLookupResult find_file_node(std::string_view path) {
    const auto lease = common::acquire_connection_lease("find_file_node");
    if (!lease.has_value()) {
        return {.status = FileLookupStatus::InternalError, .node = {}};
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = tx.exec_params(
            "SELECT n.path, n.node_type, n.sha256, n.size_bytes, o.object_rel_path "
            "FROM storage_nodes n LEFT JOIN storage_objects o ON n.sha256 = o.sha256 "
            "WHERE n.path = $1 LIMIT 1",
            std::string(path));
        if (rows.empty()) {
            return {.status = FileLookupStatus::NotFound, .node = {}};
        }

        const pqxx::row row = rows.front();
        if (require_node_type(row[1].as<std::string>()) == StorageNodeType::Directory) {
            return {.status = FileLookupStatus::Directory, .node = {}};
        }
        if (row[2].is_null() || row[3].is_null() || row[4].is_null()) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "find file node failed")
                .field("path", path)
                .field("error", "file_node_object_missing")
                .field("decision", "return_internal_error");
            return {.status = FileLookupStatus::InternalError, .node = {}};
        }
        FileNodeRecord node{
            .path = row[0].as<std::string>(),
            .sha256 = row[2].as<std::string>(),
            .size_bytes = row[3].as<std::int64_t>(0),
            .object_rel_path = row[4].as<std::string>(),
        };
        return {.status = FileLookupStatus::Found, .node = std::move(node)};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "find file node failed")
            .field("path", path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = FileLookupStatus::InternalError, .node = {}};
    }
}

CreateDirectoryResult create_directory_node(std::int64_t user_id, std::string_view scoped_path, std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("create_directory_node");
    if (!lease.has_value()) {
        return {.status = CreateDirectoryStatus::InternalError};
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s);

        if (!is_user_scoped_storage_path(user_id, scoped_path)) {
            return {.status = CreateDirectoryStatus::ParentNotFound};
        }
        if (scoped_path == user_root_path(user_id)) {
            tx.commit();
            return {.status = CreateDirectoryStatus::AlreadyExists};
        }

        const ParentDirectoryStatus parent_status = validate_parent_directory(tx, scoped_path, user_id);
        switch (parent_status) {
            case ParentDirectoryStatus::Ready:
                break;
            case ParentDirectoryStatus::NotFound:
                return {.status = CreateDirectoryStatus::ParentNotFound};
            case ParentDirectoryStatus::NotDirectory:
                return {.status = CreateDirectoryStatus::ParentNotDirectory};
        }

        const std::optional<StorageNodeType> existing_type = find_node_type_for_update(tx, scoped_path);
        if (existing_type.has_value()) {
            tx.commit();
            return {.status = *existing_type == StorageNodeType::Directory ? CreateDirectoryStatus::AlreadyExists
                                                                           : CreateDirectoryStatus::PathConflict};
        }

        tx.exec_params(
            "INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s) "
            "VALUES($1, 'directory', NULL, NULL, $2)",
            std::string(scoped_path), now_s);
        tx.commit();
        return {.status = CreateDirectoryStatus::Created};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "create directory node failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = CreateDirectoryStatus::InternalError};
    }
}

DirectoryListResult list_directory_children(std::int64_t user_id, std::string_view scoped_path, std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("list_directory_children");
    if (!lease.has_value()) {
        return {.status = DirectoryListStatus::InternalError, .items = {}};
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s);

        const std::optional<StorageNodeType> node_type = find_node_type_for_update(tx, scoped_path);
        if (!node_type.has_value()) {
            return {.status = DirectoryListStatus::NotFound, .items = {}};
        }
        if (*node_type != StorageNodeType::Directory) {
            return {.status = DirectoryListStatus::NotDirectory, .items = {}};
        }

        const std::string prefix = std::format("{}/", scoped_path);
        const pqxx::result rows = tx.exec_params(
            "SELECT path, node_type, size_bytes FROM storage_nodes "
            "WHERE path LIKE $1 ESCAPE '\\' ORDER BY path",
            build_prefix_like_pattern(prefix));

        std::vector<TreeItem> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            const auto path = row[0].as<std::string>();
            if (!path.starts_with(prefix)) {
                continue;
            }
            const std::string_view relative = std::string_view(path).substr(prefix.size());
            if (relative.empty() || relative.find('/') != std::string_view::npos) {
                continue;
            }

            const StorageNodeType child_type = require_node_type(row[1].as<std::string>());
            TreeItem item;
            item.name = std::string(relative);
            item.is_directory = child_type == StorageNodeType::Directory;
            if (!item.is_directory && !row[2].is_null()) {
                item.size_bytes = row[2].as<std::int64_t>(0);
            }
            items.push_back(std::move(item));
        }

        tx.commit();
        return {.status = DirectoryListStatus::Listed, .items = std::move(items)};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "list directory children failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = DirectoryListStatus::InternalError, .items = {}};
    }
}

UploadSessionCreateResult create_upload_session(const UploadSessionRecord& session, std::int64_t user_id,
                                                std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("upload_init");
    if (!lease.has_value()) {
        return {.status = UploadSessionCreateStatus::InternalError};
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s);

        if (!is_user_scoped_storage_path(user_id, session.path)) {
            return {.status = UploadSessionCreateStatus::ParentNotFound};
        }

        const ParentDirectoryStatus parent_status = validate_parent_directory(tx, session.path, user_id);
        switch (parent_status) {
            case ParentDirectoryStatus::Ready:
                break;
            case ParentDirectoryStatus::NotFound:
                return {.status = UploadSessionCreateStatus::ParentNotFound};
            case ParentDirectoryStatus::NotDirectory:
                return {.status = UploadSessionCreateStatus::ParentNotDirectory};
        }

        const std::optional<StorageNodeType> existing_type = find_node_type_for_update(tx, session.path);
        if (existing_type.has_value() && *existing_type == StorageNodeType::Directory) {
            return {.status = UploadSessionCreateStatus::PathConflict};
        }
        if (has_descendant_nodes(tx, session.path)) {
            return {.status = UploadSessionCreateStatus::PathConflict};
        }

        lock_storage_temp_path_sync(tx, session.temp_rel_path);
        const pqxx::result inserted = tx.exec_params(
            "INSERT INTO storage_upload_sessions(upload_id, path, temp_rel_path, total_chunks, next_chunk_index, "
            "temp_size_bytes, created_at_s, updated_at_s) VALUES($1, $2, $3, $4, 0, $5, $6, $6) "
            "ON CONFLICT (upload_id) DO NOTHING RETURNING upload_id",
            session.upload_id, session.path, session.temp_rel_path, session.total_chunks, session.temp_size_bytes,
            now_s);
        if (inserted.empty()) {
            return {.status = UploadSessionCreateStatus::InternalError};
        }
        tx.commit();
        return {.status = UploadSessionCreateStatus::Created};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "create upload session failed")
            .field("upload_id", session.upload_id)
            .field("path", session.path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = UploadSessionCreateStatus::InternalError};
    }
}

UploadChunkAppendResult append_upload_chunk(
    std::string_view upload_id, std::int64_t user_id, std::int64_t chunk_index, std::int64_t now_s,
    const std::filesystem::path& storage_root_dir,
    const std::function<UploadChunkFileAppendResult(const std::filesystem::path&, std::int64_t)>& append_chunk_file,
    const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file) {
    const auto lease = common::acquire_connection_lease("upload_chunk");
    if (!lease.has_value()) {
        return {.status = UploadChunkAppendStatus::InternalError, .total_chunks = 0, .next_chunk_index = 0};
    }

    std::filesystem::path temp_abs_path;
    std::int64_t committed_size_bytes = 0;
    bool file_appended = false;
    try {
        pqxx::work tx(lease->connection());
        const pqxx::result rows = tx.exec_params(
            "SELECT path, temp_rel_path, total_chunks, next_chunk_index, temp_size_bytes "
            "FROM storage_upload_sessions WHERE upload_id = $1 FOR UPDATE",
            std::string(upload_id));
        if (rows.empty()) {
            return {.status = UploadChunkAppendStatus::NotFound, .total_chunks = 0, .next_chunk_index = 0};
        }

        const pqxx::row row = rows.front();
        const auto session_path = row[0].as<std::string>();
        if (!is_user_scoped_storage_path(user_id, session_path)) {
            return {.status = UploadChunkAppendStatus::NotFound, .total_chunks = 0, .next_chunk_index = 0};
        }

        const auto total_chunks = row[2].as<std::int64_t>(0);
        const auto next_chunk_index = row[3].as<std::int64_t>(0);
        if (next_chunk_index >= total_chunks) {
            return {.status = UploadChunkAppendStatus::AlreadyComplete,
                    .total_chunks = total_chunks,
                    .next_chunk_index = next_chunk_index};
        }
        if (next_chunk_index != chunk_index) {
            return {.status = UploadChunkAppendStatus::InvalidChunkIndex,
                    .total_chunks = total_chunks,
                    .next_chunk_index = next_chunk_index};
        }

        committed_size_bytes = row[4].as<std::int64_t>(-1);
        if (committed_size_bytes < 0) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "append upload chunk failed")
                .field("upload_id", upload_id)
                .field("chunk_index", chunk_index)
                .field("error", "temp_size_missing")
                .field("decision", "return_internal_error");
            return {.status = UploadChunkAppendStatus::InternalError,
                    .total_chunks = total_chunks,
                    .next_chunk_index = next_chunk_index};
        }
        temp_abs_path = storage_root_dir / row[1].as<std::string>();
        const UploadChunkFileAppendResult append_result = append_chunk_file(temp_abs_path, committed_size_bytes);
        switch (append_result.status) {
            case UploadChunkFileAppendStatus::Appended:
                break;
            case UploadChunkFileAppendStatus::FileTooLarge:
                restore_after_append_failure(upload_id, chunk_index, temp_abs_path, committed_size_bytes,
                                             restore_chunk_file);
                return {.status = UploadChunkAppendStatus::FileTooLarge,
                        .total_chunks = total_chunks,
                        .next_chunk_index = next_chunk_index};
            case UploadChunkFileAppendStatus::Failed:
                restore_after_append_failure(upload_id, chunk_index, temp_abs_path, committed_size_bytes,
                                             restore_chunk_file);
                return {.status = UploadChunkAppendStatus::InternalError,
                        .total_chunks = total_chunks,
                        .next_chunk_index = next_chunk_index};
        }
        if (append_result.bytes_written < 0 ||
            append_result.bytes_written > (std::numeric_limits<std::int64_t>::max() - committed_size_bytes)) {
            restore_after_append_failure(upload_id, chunk_index, temp_abs_path, committed_size_bytes,
                                         restore_chunk_file);
            return {.status = UploadChunkAppendStatus::InternalError,
                    .total_chunks = total_chunks,
                    .next_chunk_index = next_chunk_index};
        }
        file_appended = true;
        const std::int64_t new_size_bytes = committed_size_bytes + append_result.bytes_written;

        const pqxx::result updated = tx.exec_params(
            "UPDATE storage_upload_sessions "
            "SET next_chunk_index = next_chunk_index + 1, temp_size_bytes = $3, updated_at_s = $2 "
            "WHERE upload_id = $1 RETURNING next_chunk_index",
            std::string(upload_id), now_s, new_size_bytes);
        if (updated.empty()) {
            throw std::runtime_error("upload_session_update_missing");
        }
        tx.commit();
        file_appended = false;
        return {.status = UploadChunkAppendStatus::Advanced,
                .total_chunks = total_chunks,
                .next_chunk_index = next_chunk_index + 1};
    } catch (const std::exception& e) {
        if (file_appended && !temp_abs_path.empty() && !restore_chunk_file(temp_abs_path, committed_size_bytes)) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "upload chunk file restore failed")
                .field("upload_id", upload_id)
                .field("chunk_index", chunk_index)
                .field("path", temp_abs_path.string())
                .field("error", "restore_failed")
                .field("decision", "retry_on_next_chunk_upload");
        }
        common::Logger::instance()
            .error(common::LogDomain::Storage, "append upload chunk failed")
            .field("upload_id", upload_id)
            .field("chunk_index", chunk_index)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = UploadChunkAppendStatus::InternalError, .total_chunks = 0, .next_chunk_index = 0};
    }
}

UploadCompletePrepareResult prepare_upload_complete(std::string_view upload_id, std::int64_t user_id) {
    const auto lease = common::acquire_connection_lease("upload_complete_prepare");
    if (!lease.has_value()) {
        return {.status = UploadCompletePrepareStatus::InternalError,
                .scoped_path = {},
                .temp_rel_path = {},
                .temp_size_bytes = 0};
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = tx.exec_params(
            "SELECT path, temp_rel_path, total_chunks, next_chunk_index, temp_size_bytes "
            "FROM storage_upload_sessions WHERE upload_id = $1 LIMIT 1",
            std::string(upload_id));
        if (rows.empty()) {
            return {.status = UploadCompletePrepareStatus::NotFound,
                    .scoped_path = {},
                    .temp_rel_path = {},
                    .temp_size_bytes = 0};
        }

        const pqxx::row row = rows.front();
        const auto path = row[0].as<std::string>();
        if (!is_user_scoped_storage_path(user_id, path)) {
            return {.status = UploadCompletePrepareStatus::NotFound,
                    .scoped_path = {},
                    .temp_rel_path = {},
                    .temp_size_bytes = 0};
        }

        const auto total_chunks = row[2].as<std::int64_t>(0);
        const auto next_chunk_index = row[3].as<std::int64_t>(0);
        if (next_chunk_index != total_chunks) {
            return {.status = UploadCompletePrepareStatus::Incomplete,
                    .scoped_path = {},
                    .temp_rel_path = {},
                    .temp_size_bytes = 0};
        }
        const auto temp_size_bytes = row[4].as<std::int64_t>(-1);
        if (temp_size_bytes < 0) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "prepare upload complete failed")
                .field("upload_id", upload_id)
                .field("error", "temp_size_missing")
                .field("decision", "return_internal_error");
            return {.status = UploadCompletePrepareStatus::InternalError,
                    .scoped_path = {},
                    .temp_rel_path = {},
                    .temp_size_bytes = 0};
        }

        return {.status = UploadCompletePrepareStatus::Ready,
                .scoped_path = path,
                .temp_rel_path = row[1].as<std::string>(),
                .temp_size_bytes = temp_size_bytes};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "prepare upload complete failed")
            .field("upload_id", upload_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = UploadCompletePrepareStatus::InternalError,
                .scoped_path = {},
                .temp_rel_path = {},
                .temp_size_bytes = 0};
    }
}

UploadCompleteFinalizeResult finalize_upload_complete(std::string_view upload_id, std::int64_t user_id,
                                                      std::string_view sha256, std::int64_t size_bytes,
                                                      std::string_view object_rel_path, std::int64_t now_s,
                                                      const std::function<bool()>& publish_object_file) {
    const auto lease = common::acquire_connection_lease("upload_complete_finalize");
    if (!lease.has_value()) {
        return {
            .status = UploadCompleteFinalizeStatus::InternalError, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_shared(tx);
        if (!publish_object_file()) {
            common::Logger::instance()
                .error(common::LogDomain::Storage, "finalize upload complete failed")
                .field("upload_id", upload_id)
                .field("error", "object_file_publish_failed")
                .field("decision", "return_internal_error");
            return {.status = UploadCompleteFinalizeStatus::InternalError,
                    .scoped_path = {},
                    .cleanup_old_sha = std::nullopt};
        }

        UploadCompleteFinalizeResult result =
            finalize_upload_complete_in_tx(tx, upload_id, user_id, sha256, size_bytes, object_rel_path, now_s);
        if (result.status == UploadCompleteFinalizeStatus::Completed) {
            tx.commit();
        }
        return result;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "finalize upload complete failed")
            .field("upload_id", upload_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {
            .status = UploadCompleteFinalizeStatus::InternalError, .scoped_path = {}, .cleanup_old_sha = std::nullopt};
    }
}

DeleteNodeResult delete_node_record(std::int64_t user_id, std::string_view scoped_path, std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("delete_node");
    if (!lease.has_value()) {
        return {.status = DeleteNodeStatus::InternalError, .cleanup_sha = std::nullopt};
    }

    try {
        std::optional<std::string> cleanup_sha;

        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s);
        if (!is_user_scoped_storage_path(user_id, scoped_path)) {
            return {.status = DeleteNodeStatus::NotFound, .cleanup_sha = std::nullopt};
        }

        const pqxx::result existing_rows = tx.exec_params(
            "SELECT node_type, sha256 FROM storage_nodes WHERE path = $1 FOR UPDATE", std::string(scoped_path));
        if (existing_rows.empty()) {
            return {.status = DeleteNodeStatus::NotFound, .cleanup_sha = std::nullopt};
        }

        const pqxx::row existing_row = existing_rows.front();
        if (require_node_type(existing_row[0].as<std::string>()) == StorageNodeType::File) {
            if (existing_row[1].is_null()) {
                throw std::runtime_error("file_node_sha_missing");
            }
            const auto sha256 = existing_row[1].as<std::string>();
            tx.exec_params("DELETE FROM storage_nodes WHERE path = $1", std::string(scoped_path));
            const pqxx::result ref_rows = tx.exec_params(
                "UPDATE storage_objects SET ref_count = ref_count - 1, updated_at_s = $2 "
                "WHERE sha256 = $1 RETURNING ref_count",
                sha256, now_s);
            if (!ref_rows.empty() && ref_rows.front()[0].as<std::int64_t>(0) <= 0) {
                cleanup_sha = sha256;
            }
            tx.commit();
            return {.status = DeleteNodeStatus::FileDeleted, .cleanup_sha = std::move(cleanup_sha)};
        }

        const std::string prefix = std::format("{}/", scoped_path);
        const pqxx::result child_rows = tx.exec_params(
            "SELECT 1 FROM storage_nodes WHERE path LIKE $1 ESCAPE '\\' LIMIT 1", build_prefix_like_pattern(prefix));
        if (!child_rows.empty()) {
            return {.status = DeleteNodeStatus::NonEmptyDirectory, .cleanup_sha = std::nullopt};
        }

        tx.exec_params("DELETE FROM storage_nodes WHERE path = $1", std::string(scoped_path));
        tx.commit();
        return {.status = DeleteNodeStatus::DirectoryDeleted, .cleanup_sha = std::nullopt};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "delete node record failed")
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = DeleteNodeStatus::InternalError, .cleanup_sha = std::nullopt};
    }
}

StorageGcSnapshot collect_storage_gc_snapshot(std::int64_t expired_before_s, std::int64_t now_s) {
    const auto lease = common::acquire_connection_lease("storage_gc");
    if (!lease.has_value()) {
        return {.status = StorageGcSnapshotStatus::InternalError,
                .expired_sessions = {},
                .active_temp_rel_paths = {},
                .object_rel_paths_in_db = {},
                .orphan_objects = {}};
    }

    try {
        StorageGcSnapshot snapshot;
        snapshot.status = StorageGcSnapshotStatus::Ready;

        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);

        const pqxx::result expired_rows = tx.exec_params(
            "SELECT upload_id, temp_rel_path FROM storage_upload_sessions WHERE updated_at_s < $1", expired_before_s);
        snapshot.expired_sessions.reserve(expired_rows.size());
        for (const auto& row : expired_rows) {
            snapshot.expired_sessions.push_back(GcExpiredUploadSession{.upload_id = row[0].as<std::string>(),
                                                                       .temp_rel_path = row[1].as<std::string>()});
        }
        tx.exec_params("DELETE FROM storage_upload_sessions WHERE updated_at_s < $1", expired_before_s);

        const pqxx::result active_temp_rows = tx.exec("SELECT temp_rel_path FROM storage_upload_sessions");
        snapshot.active_temp_rel_paths.reserve(active_temp_rows.size());
        for (const auto& row : active_temp_rows) {
            snapshot.active_temp_rel_paths.push_back(row[0].as<std::string>());
        }

        tx.exec_params(
            "UPDATE storage_objects o SET ref_count = c.ref_cnt, updated_at_s = $1 "
            "FROM (SELECT sha256, COUNT(*)::bigint AS ref_cnt FROM storage_nodes "
            "WHERE node_type = 'file' GROUP BY sha256) c "
            "WHERE o.sha256 = c.sha256 AND o.ref_count <> c.ref_cnt",
            now_s);
        tx.exec_params(
            "UPDATE storage_objects o SET ref_count = 0, updated_at_s = $1 "
            "WHERE NOT EXISTS (SELECT 1 FROM storage_nodes n WHERE n.node_type = 'file' AND n.sha256 = o.sha256) "
            "AND o.ref_count <> 0",
            now_s);

        const pqxx::result object_path_rows = tx.exec("SELECT object_rel_path FROM storage_objects");
        snapshot.object_rel_paths_in_db.reserve(object_path_rows.size());
        for (const auto& row : object_path_rows) {
            snapshot.object_rel_paths_in_db.push_back(row[0].as<std::string>());
        }

        const pqxx::result orphan_rows =
            tx.exec("SELECT sha256, object_rel_path FROM storage_objects WHERE ref_count <= 0");
        snapshot.orphan_objects.reserve(orphan_rows.size());
        for (const auto& row : orphan_rows) {
            snapshot.orphan_objects.push_back(
                GcOrphanObject{.sha256 = row[0].as<std::string>(), .object_rel_path = row[1].as<std::string>()});
        }

        tx.commit();
        return snapshot;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "collect storage gc snapshot failed")
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = StorageGcSnapshotStatus::InternalError,
                .expired_sessions = {},
                .active_temp_rel_paths = {},
                .object_rel_paths_in_db = {},
                .orphan_objects = {}};
    }
}

CleanupStatus cleanup_unreferenced_object(const StorageRouteConfig& config, std::string_view sha256) {
    const auto lease = common::acquire_connection_lease("cleanup_unreferenced_object");
    if (!lease.has_value()) {
        return CleanupStatus::InternalError;
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);
        const pqxx::result rows = tx.exec_params(
            "SELECT object_rel_path, ref_count, "
            "EXISTS (SELECT 1 FROM storage_nodes n WHERE n.node_type = 'file' AND n.sha256 = $1) "
            "FROM storage_objects WHERE sha256 = $1 FOR UPDATE",
            std::string(sha256));
        if (rows.empty()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        const pqxx::row row = rows.front();
        if (row[1].as<std::int64_t>(0) > 0 || row[2].as<bool>(false)) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        const std::filesystem::path object_path = config.root_dir / row[0].as<std::string>();
        if (!delete_file_if_exists(object_path)) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "cleanup object file failed")
                .field("sha256", sha256)
                .field("path", object_path.string())
                .field("decision", "keep_orphan_row_for_gc");
            tx.commit();
            return CleanupStatus::Skipped;
        }
        try_cleanup_empty_parents(object_path, config.objects_dir);
        tx.exec_params("DELETE FROM storage_objects WHERE sha256 = $1 AND ref_count <= 0", std::string(sha256));
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Storage, "cleanup unreferenced object failed")
            .field("sha256", sha256)
            .field("error", e.what())
            .field("decision", "keep_orphan_row_for_gc");
        return CleanupStatus::InternalError;
    }
}

CleanupStatus cleanup_file_only_object(const StorageRouteConfig& config, const std::filesystem::path& object_abs_path) {
    const auto lease = common::acquire_connection_lease("cleanup_file_only_object");
    if (!lease.has_value()) {
        return CleanupStatus::InternalError;
    }

    try {
        std::error_code ec;
        const std::filesystem::path object_rel_path = std::filesystem::relative(object_abs_path, config.root_dir, ec);
        const std::string object_rel_path_str = object_rel_path.generic_string();
        if (ec || object_rel_path_str.empty() || object_rel_path_str.starts_with("../") ||
            object_rel_path_str == "..") {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "cleanup file-only orphan object failed")
                .field("path", object_abs_path.string())
                .field("error", "invalid_object_path")
                .field("decision", "keep_orphan");
            return CleanupStatus::InternalError;
        }

        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);
        const pqxx::result referenced_rows =
            tx.exec_params("SELECT 1 FROM storage_objects WHERE object_rel_path = $1 LIMIT 1", object_rel_path_str);
        if (!referenced_rows.empty()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        if (!delete_file_if_exists(object_abs_path)) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "cleanup file-only orphan object failed")
                .field("path", object_abs_path.string())
                .field("error", "object_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return CleanupStatus::Skipped;
        }
        try_cleanup_empty_parents(object_abs_path, config.objects_dir);
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn(common::LogDomain::Storage, "cleanup file-only orphan object failed")
            .field("path", object_abs_path.string())
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    }
}

CleanupStatus cleanup_temp_file(const StorageRouteConfig& config, const std::filesystem::path& temp_abs_path) {
    try {
        std::error_code ec;
        const std::filesystem::path temp_rel_path = std::filesystem::relative(temp_abs_path, config.root_dir, ec);
        const std::string temp_rel_path_str = temp_rel_path.generic_string();
        if (ec || temp_rel_path_str.empty() || temp_rel_path_str.starts_with("../") || temp_rel_path_str == ".." ||
            !temp_rel_path_str.starts_with("temp/")) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "cleanup temp file failed")
                .field("path", temp_abs_path.string())
                .field("error", "invalid_temp_path")
                .field("decision", "keep_orphan");
            return CleanupStatus::InternalError;
        }

        const auto lease = common::acquire_connection_lease("cleanup_temp_file");
        if (!lease.has_value()) {
            return CleanupStatus::InternalError;
        }

        pqxx::work tx(lease->connection());
        lock_storage_temp_path_sync(tx, temp_rel_path_str);
        const pqxx::result referenced_rows =
            tx.exec_params("SELECT 1 FROM storage_upload_sessions WHERE temp_rel_path = $1 LIMIT 1", temp_rel_path_str);
        if (!referenced_rows.empty()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        if (!delete_file_if_exists(temp_abs_path)) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "cleanup temp file failed")
                .field("path", temp_abs_path.string())
                .field("error", "temp_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return CleanupStatus::Skipped;
        }
        try_cleanup_empty_parents(temp_abs_path, config.temp_dir);
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn(common::LogDomain::Storage, "cleanup temp file failed")
            .field("path", temp_abs_path.string())
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    }
}

void cleanup_upload_failure_object(const StorageRouteConfig& config, std::string_view upload_id,
                                   std::string_view sha256, const std::filesystem::path& object_abs_path) {
    if (sha256.empty() || object_abs_path.empty()) {
        return;
    }

    const auto lease = common::acquire_connection_lease("cleanup_upload_complete_failure");
    if (!lease.has_value()) {
        return;
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);
        const pqxx::result referenced_rows = tx.exec_params(
            "SELECT 1 WHERE "
            "EXISTS (SELECT 1 FROM storage_objects WHERE sha256 = $1 AND ref_count > 0) "
            "OR EXISTS (SELECT 1 FROM storage_nodes WHERE node_type = 'file' AND sha256 = $1)",
            std::string(sha256));
        if (!referenced_rows.empty()) {
            tx.commit();
            return;
        }

        if (!delete_file_if_exists(object_abs_path)) {
            common::Logger::instance()
                .warn(common::LogDomain::Storage, "upload complete rollback object cleanup failed")
                .field("upload_id", upload_id)
                .field("sha256", sha256)
                .field("path", object_abs_path.string())
                .field("error", "object_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return;
        }
        try_cleanup_empty_parents(object_abs_path, config.objects_dir);
        tx.commit();
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn(common::LogDomain::Storage, "upload complete rollback object cleanup failed")
            .field("upload_id", upload_id)
            .field("sha256", sha256)
            .field("path", object_abs_path.string())
            .field("error", e.what())
            .field("decision", "keep_orphan");
    }
}

}  // namespace nebula::storage
