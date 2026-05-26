#include "nebula/storage/repository/repository.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/time.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula/storage/domain/error.hpp"
#include "nebula/storage/repository/download_tickets_executor.hpp"
#include "nebula/storage/repository/download_tickets_row_parser.hpp"
#include "nebula/storage/repository/file_nodes_executor.hpp"
#include "nebula/storage/repository/file_nodes_row_parser.hpp"
#include "nebula/storage/repository/gc_executor.hpp"
#include "nebula/storage/repository/gc_row_parser.hpp"
#include "nebula/storage/repository/schema_executor.hpp"
#include "nebula/storage/repository/schema_row_parser.hpp"
#include "nebula/storage/repository/upload_sessions_executor.hpp"
#include "nebula/storage/repository/upload_sessions_row_parser.hpp"

namespace nebula::storage {

namespace {

enum class StorageQuotaCheckStatus : std::uint8_t {
    Allowed,
    QuotaExceeded,
    InternalError,
};

enum class ExistingFileTargetStatus : std::uint8_t {
    Ready,
    PathConflict,
    InternalError,
};

struct ExistingFileTargetState {
    bool exists = false;
    std::int64_t replaced_size_bytes = 0;
    std::string old_sha;
};

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

[[nodiscard]] std::string parent_storage_path(std::string_view path) {
    const std::size_t slash_pos = path.rfind('/');
    if (slash_pos == std::string_view::npos || slash_pos == 0U) {
        return {};
    }
    const std::string_view parent = path.substr(0, slash_pos);
    return std::string(parent);
}

void lock_storage_object_gc_sync_exclusive(pqxx::work& tx) {
    execute_acquire_global_storage_object_gc_exclusive_advisory_lock(tx);
}

void lock_storage_object_gc_sync_shared(pqxx::work& tx) {
    execute_acquire_global_storage_object_gc_shared_advisory_lock(tx);
}

void lock_storage_tree_sync(pqxx::work& tx, std::int64_t user_id) {
    execute_acquire_storage_tree_advisory_lock(tx, user_id);
}

void lock_storage_temp_path_sync(pqxx::work& tx, std::string_view temp_rel_path) {
    execute_acquire_storage_temp_path_advisory_lock(tx, temp_rel_path);
}

void ensure_user_root_directory_in_tx(pqxx::work& tx, std::int64_t user_id, std::int64_t now_s) {
    execute_ensure_user_root_directory(tx, user_id, now_s);
}

[[nodiscard]] std::optional<StorageNodeType> find_node_type_for_update(pqxx::work& tx, std::string_view path) {
    const pqxx::result rows = execute_find_node_type_for_update(tx, path);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<StorageNodeType> node_type = parse_node_type_row(rows.one_row());
    if (!node_type.has_value()) {
        throw std::runtime_error("storage_node_type_missing");
    }
    return node_type;
}

[[nodiscard]] std::expected<void, StorageError> validate_parent_directory(pqxx::work& tx, std::string_view scoped_path,
                                                                          std::int64_t user_id) {
    const std::string parent = parent_storage_path(scoped_path);
    if (parent.empty() || !is_user_scoped_storage_path(user_id, parent)) {
        return std::unexpected(StorageError::ParentNotFound);
    }

    const std::optional<StorageNodeType> parent_type = find_node_type_for_update(tx, parent);
    if (!parent_type.has_value()) {
        return std::unexpected(StorageError::ParentNotFound);
    }
    if (*parent_type != StorageNodeType::Directory) {
        return std::unexpected(StorageError::ParentNotDirectory);
    }
    return {};
}

[[nodiscard]] bool has_descendant_nodes(pqxx::work& tx, std::string_view path) {
    return !execute_find_storage_node_descendant(tx, std::format("{}/", path)).empty();
}

[[nodiscard]] std::optional<std::int64_t> find_user_quota_bytes(pqxx::transaction_base& tx, std::int64_t user_id) {
    const pqxx::result rows = execute_find_user_quota_bytes(tx, user_id);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<std::int64_t> quota_bytes = parse_quota_bytes_row(rows.one_row());
    if (!quota_bytes.has_value() || *quota_bytes <= 0) {
        return std::nullopt;
    }
    return quota_bytes;
}

[[nodiscard]] std::int64_t sum_user_file_bytes(pqxx::transaction_base& tx, std::int64_t user_id) {
    const std::optional<std::int64_t> sum_bytes = parse_total_size_bytes_row(execute_sum_user_file_bytes(tx, user_id));
    return sum_bytes.value_or(0);
}

[[nodiscard]] StorageQuotaCheckStatus check_user_storage_quota_for_write(pqxx::transaction_base& tx,
                                                                         std::int64_t user_id,
                                                                         std::int64_t replaced_size_bytes,
                                                                         std::int64_t size_bytes) {
    const std::optional<std::int64_t> quota_bytes = find_user_quota_bytes(tx, user_id);
    if (!quota_bytes.has_value()) {
        return StorageQuotaCheckStatus::InternalError;
    }

    const std::int64_t used_bytes = sum_user_file_bytes(tx, user_id);
    if (used_bytes < replaced_size_bytes ||
        size_bytes > (std::numeric_limits<std::int64_t>::max() - used_bytes + replaced_size_bytes)) {
        return StorageQuotaCheckStatus::InternalError;
    }

    const std::int64_t projected_used_bytes = used_bytes - replaced_size_bytes + size_bytes;
    if (projected_used_bytes > *quota_bytes) {
        return StorageQuotaCheckStatus::QuotaExceeded;
    }
    return StorageQuotaCheckStatus::Allowed;
}

[[nodiscard]] std::pair<ExistingFileTargetStatus, ExistingFileTargetState> load_existing_file_target_state(
    pqxx::work& tx, std::string_view path) {
    const pqxx::result rows = execute_find_existing_file_target_for_update(tx, path);
    if (rows.empty()) {
        return {ExistingFileTargetStatus::Ready, ExistingFileTargetState{}};
    }

    const std::optional<ExistingFileTargetRow> parsed = parse_existing_file_target_row(rows.one_row());
    if (!parsed.has_value()) {
        return {ExistingFileTargetStatus::InternalError, ExistingFileTargetState{}};
    }
    if (parsed->node_type == StorageNodeType::Directory) {
        return {ExistingFileTargetStatus::PathConflict, ExistingFileTargetState{}};
    }
    if (!parsed->sha256.has_value()) {
        return {ExistingFileTargetStatus::InternalError, ExistingFileTargetState{}};
    }

    ExistingFileTargetState state;
    state.exists = true;
    state.old_sha = *parsed->sha256;
    state.replaced_size_bytes = parsed->size_bytes.value_or(0);
    return {ExistingFileTargetStatus::Ready, std::move(state)};
}

[[nodiscard]] std::optional<UploadSessionChunkRow> find_upload_session_chunk_row_for_update(
    pqxx::work& tx, std::string_view upload_id) {
    const pqxx::result rows = execute_find_upload_session_for_append(tx, upload_id);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<UploadSessionChunkRow> parsed = parse_upload_session_chunk_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("upload_session_row_invalid");
    }
    return parsed;
}

[[nodiscard]] std::optional<UploadSessionChunkRow> find_upload_session_chunk_row(pqxx::read_transaction& tx,
                                                                                 std::string_view upload_id) {
    const pqxx::result rows = execute_find_upload_session_for_prepare(tx, upload_id);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<UploadSessionChunkRow> parsed = parse_upload_session_chunk_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("upload_session_row_invalid");
    }
    return parsed;
}

[[nodiscard]] std::optional<UploadSessionFinalizeRow> find_upload_session_finalize_row_for_update(
    pqxx::work& tx, std::string_view upload_id) {
    const pqxx::result rows = execute_find_upload_session_for_finalize(tx, upload_id);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<UploadSessionFinalizeRow> parsed = parse_upload_session_finalize_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("upload_session_row_invalid");
    }
    return parsed;
}

[[nodiscard]] std::optional<NodeDeleteRow> find_node_delete_row_for_update(pqxx::work& tx,
                                                                           std::string_view scoped_path) {
    const pqxx::result rows = execute_find_node_for_delete(tx, scoped_path);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<NodeDeleteRow> parsed = parse_node_delete_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("storage_node_row_invalid");
    }
    return parsed;
}

[[nodiscard]] std::optional<DownloadTicketRow> find_download_ticket_row(pqxx::read_transaction& tx,
                                                                        std::string_view ticket) {
    const pqxx::result rows = execute_find_download_ticket(tx, ticket);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<DownloadTicketRow> parsed = parse_download_ticket_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("download_ticket_row_invalid");
    }
    return parsed;
}

[[nodiscard]] std::optional<UnreferencedObjectCleanupRow> find_cleanup_candidate_object_row_for_update(
    pqxx::work& tx, std::string_view sha256) {
    const pqxx::result rows = execute_find_cleanup_candidate_object(tx, sha256);
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::optional<UnreferencedObjectCleanupRow> parsed = parse_unreferenced_object_cleanup_row(rows.one_row());
    if (!parsed.has_value()) {
        throw std::runtime_error("storage_object_row_invalid");
    }
    return parsed;
}

void restore_after_append_failure(
    std::string_view upload_id, std::int64_t chunk_index, const std::filesystem::path& temp_abs_path,
    std::int64_t committed_size_bytes,
    const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file) {
    if (restore_chunk_file(temp_abs_path, committed_size_bytes)) {
        return;
    }

    common::Logger::instance()
        .warn("upload temp file restore failed")
        .field("upload_id", upload_id)
        .field("chunk_index", chunk_index)
        .field("path", temp_abs_path)
        .field("error", "restore_failed")
        .field("decision", "retry_on_next_chunk_upload");
}

std::expected<UploadCompleteFinalizeInfo, StorageError> finalize_upload_complete_in_tx(
    pqxx::work& tx, std::string_view upload_id, std::int64_t user_id, std::string_view sha256, std::int64_t size_bytes,
    std::string_view object_rel_path, std::chrono::seconds now_s) {
    std::optional<std::string> cleanup_old_sha;
    const std::optional<UploadSessionFinalizeRow> session = find_upload_session_finalize_row_for_update(tx, upload_id);
    if (!session.has_value()) {
        return std::unexpected(StorageError::UploadNotFound);
    }
    if (!is_user_scoped_storage_path(user_id, session->path)) {
        return std::unexpected(StorageError::UploadNotFound);
    }
    if (session->next_chunk_index != session->total_chunks) {
        return std::unexpected(StorageError::UploadIncomplete);
    }
    if (!session->temp_size_bytes.has_value() || *session->temp_size_bytes < 0 ||
        size_bytes != *session->temp_size_bytes) {
        common::Logger::instance()
            .error("finalize upload complete failed")
            .field("upload_id", upload_id)
            .field("error", !session->temp_size_bytes.has_value() || *session->temp_size_bytes < 0
                                ? "temp_size_missing"
                                : "temp_size_mismatch")
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }

    lock_storage_tree_sync(tx, user_id);
    ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

    const auto parent_check = validate_parent_directory(tx, session->path, user_id);
    if (!parent_check.has_value()) {
        return std::unexpected(parent_check.error());
    }

    if (has_descendant_nodes(tx, session->path)) {
        return std::unexpected(StorageError::PathConflict);
    }

    const auto [existing_target_status, existing_target] = load_existing_file_target_state(tx, session->path);
    switch (existing_target_status) {
        case ExistingFileTargetStatus::Ready:
            break;
        case ExistingFileTargetStatus::PathConflict:
            return std::unexpected(StorageError::PathConflict);
        case ExistingFileTargetStatus::InternalError:
            throw std::runtime_error("file_node_sha_missing");
    }

    switch (check_user_storage_quota_for_write(tx, user_id, existing_target.replaced_size_bytes, size_bytes)) {
        case StorageQuotaCheckStatus::Allowed:
            break;
        case StorageQuotaCheckStatus::QuotaExceeded:
            return std::unexpected(StorageError::StorageQuotaExceeded);
        case StorageQuotaCheckStatus::InternalError:
            common::Logger::instance()
                .error("finalize upload complete failed")
                .field("user_id", user_id)
                .field("path", session->path)
                .field("error", "user_quota_check_failed")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
    }

    if (!existing_target.exists) {
        execute_upsert_storage_object(tx, sha256, size_bytes, object_rel_path, now_s.count());
        execute_insert_file_node(tx, session->path, sha256, size_bytes, now_s.count());
    } else if (existing_target.old_sha != sha256) {
        execute_upsert_storage_object(tx, sha256, size_bytes, object_rel_path, now_s.count());
        execute_update_file_node_with_object(tx, session->path, sha256, size_bytes, now_s.count());
        const pqxx::result old_object_rows =
            execute_decrement_storage_object_ref_count(tx, existing_target.old_sha, now_s.count());
        if (!old_object_rows.empty()) {
            const std::optional<std::int64_t> ref_count = parse_storage_object_ref_count_row(old_object_rows.one_row());
            if (ref_count.has_value() && *ref_count <= 0) {
                cleanup_old_sha = existing_target.old_sha;
            }
        }
    } else {
        execute_update_file_node_size(tx, session->path, size_bytes, now_s.count());
    }

    execute_delete_upload_session(tx, upload_id);
    return UploadCompleteFinalizeInfo{.scoped_path = session->path, .cleanup_old_sha = std::move(cleanup_old_sha)};
}

}  // namespace

StorageRepository::StorageRepository(std::shared_ptr<database::ConnectionPool> database_pool)
    : database_pool_(std::move(database_pool)) {
    if (database_pool_ == nullptr) {
        throw std::invalid_argument("database_pool_missing");
    }
}

bool StorageRepository::check_schema_ready() {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return false;
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::row row = execute_check_storage_repository_schema_ready(tx);

        const database::RowCheckStatus row_status = parse_storage_repository_schema_ready_row(row);
        switch (row_status) {
            case database::RowCheckStatus::InvalidSize:
                common::Logger::instance()
                    .error("storage repository readiness check failed")
                    .field("error", "repository_check_result_invalid")
                    .field("decision", "return_not_ready");
                return false;
            case database::RowCheckStatus::NullField:
                common::Logger::instance()
                    .error("storage repository readiness check failed")
                    .field("error", "repository_object_missing")
                    .field("decision", "return_not_ready");
                return false;
            case database::RowCheckStatus::Ready:
                common::Logger::instance().info("storage repository is ready");
                return true;
        }

        common::Logger::instance()
            .error("storage repository readiness check failed")
            .field("error", "unknown_status")
            .field("decision", "return_not_ready");
        return false;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("storage repository readiness check failed")
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_not_ready");
        return false;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("storage repository readiness check failed")
            .field("error", e.what())
            .field("decision", "return_not_ready");
        return false;
    }
}

bool StorageRepository::ensure_user_root_directory(std::int64_t user_id) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return false;
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());
        tx.commit();
        return true;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("ensure user root directory failed")
            .field("user_id", user_id)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return false;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("ensure user root directory failed")
            .field("user_id", user_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return false;
    }
}

std::expected<FileNodeRecord, StorageError> StorageRepository::find_file_node(std::string_view path) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = execute_find_file_node(tx, path);
        if (rows.empty()) {
            return std::unexpected(StorageError::PathNotFound);
        }

        const std::optional<FileNodeLookupRow> parsed = parse_file_node_lookup_row(rows.one_row());
        if (!parsed.has_value()) {
            throw std::runtime_error("file_node_row_invalid");
        }
        if (parsed->node_type == StorageNodeType::Directory) {
            return std::unexpected(StorageError::NotFile);
        }
        if (!parsed->sha256.has_value() || !parsed->size_bytes.has_value() || !parsed->object_rel_path.has_value()) {
            common::Logger::instance()
                .error("find file node failed")
                .field("path", path)
                .field("error", "file_node_object_missing")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
        }

        FileNodeRecord node{
            .path = parsed->path,
            .sha256 = *parsed->sha256,
            .size_bytes = *parsed->size_bytes,
            .object_rel_path = *parsed->object_rel_path,
        };
        return node;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("find file node failed")
            .field("path", path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("find file node failed")
            .field("path", path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<void, StorageError> StorageRepository::create_directory_node(std::int64_t user_id,
                                                                           std::string_view scoped_path) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

        if (!is_user_scoped_storage_path(user_id, scoped_path)) {
            return std::unexpected(StorageError::ParentNotFound);
        }
        if (scoped_path == user_root_path(user_id)) {
            tx.commit();
            return std::unexpected(StorageError::DirectoryAlreadyExists);
        }

        const auto parent_check = validate_parent_directory(tx, scoped_path, user_id);
        if (!parent_check.has_value()) {
            return std::unexpected(parent_check.error());
        }

        const std::optional<StorageNodeType> existing_type = find_node_type_for_update(tx, scoped_path);
        if (existing_type.has_value()) {
            tx.commit();
            return std::unexpected(*existing_type == StorageNodeType::Directory ? StorageError::DirectoryAlreadyExists
                                                                                : StorageError::PathConflict);
        }

        execute_insert_directory_node(tx, scoped_path, now_s.count());
        tx.commit();
        return {};
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("create directory node failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("create directory node failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<std::vector<TreeItem>, StorageError> StorageRepository::list_directory_children(
    std::int64_t user_id, std::string_view scoped_path, const DirectoryListOptions& options) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

        const std::optional<StorageNodeType> node_type = find_node_type_for_update(tx, scoped_path);
        if (!node_type.has_value()) {
            return std::unexpected(StorageError::PathNotFound);
        }
        if (*node_type != StorageNodeType::Directory) {
            return std::unexpected(StorageError::NotDirectory);
        }

        const std::string prefix = std::format("{}/", scoped_path);
        const pqxx::result rows = execute_list_directory_children(tx, scoped_path, options);

        std::vector<TreeItem> items;
        items.reserve(rows.size());
        std::unordered_map<std::string, std::int64_t> file_count_by_directory;
        file_count_by_directory.reserve(rows.size());
        for (const auto& row : rows) {
            const std::optional<DirectoryChildRow> parsed = parse_directory_child_row(pqxx::row(row));
            if (!parsed.has_value()) {
                throw std::runtime_error("directory_child_row_invalid");
            }
            if (!parsed->path.starts_with(prefix)) {
                continue;
            }

            const std::string_view relative = std::string_view(parsed->path).substr(prefix.size());
            if (relative.empty()) {
                continue;
            }

            const std::size_t slash_pos = relative.find('/');
            if (slash_pos != std::string_view::npos) {
                if (parsed->node_type == StorageNodeType::File) {
                    ++file_count_by_directory[std::string(relative.substr(0, slash_pos))];
                }
                continue;
            }

            TreeItem item;
            item.name = std::string(relative);
            item.path = parsed->path;
            item.node_type = parsed->node_type;
            item.updated_at_s = parsed->updated_at_s;
            if (item.node_type == StorageNodeType::File && parsed->size_bytes.has_value()) {
                item.size_bytes = *parsed->size_bytes;
            }
            items.push_back(std::move(item));
        }

        for (TreeItem& item : items) {
            if (item.node_type != StorageNodeType::Directory) {
                continue;
            }
            const auto count_it = file_count_by_directory.find(item.name);
            if (count_it != file_count_by_directory.end()) {
                item.file_count = count_it->second;
            }
        }

        tx.commit();
        return items;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("list directory children failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("list directory children failed")
            .field("user_id", user_id)
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<std::vector<RecentFileItem>, StorageError> StorageRepository::list_recent_files(std::int64_t user_id,
                                                                                              std::int64_t limit) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

        const pqxx::result rows = execute_list_recent_files(tx, user_id, limit);

        std::vector<RecentFileItem> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            const std::optional<RecentFileItem> item = parse_recent_file_row(pqxx::row(row));
            if (!item.has_value()) {
                throw std::runtime_error("recent_file_row_invalid");
            }
            items.push_back(*item);
        }

        tx.commit();
        return items;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("list recent files failed")
            .field("user_id", user_id)
            .field("limit", limit)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("list recent files failed")
            .field("user_id", user_id)
            .field("limit", limit)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<StorageUsageInfo, StorageError> StorageRepository::collect_storage_usage(std::int64_t user_id) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

        const std::optional<std::int64_t> quota_bytes = find_user_quota_bytes(tx, user_id);
        if (!quota_bytes.has_value()) {
            common::Logger::instance()
                .error("collect storage usage failed")
                .field("user_id", user_id)
                .field("error", "user_quota_missing")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
        }

        const pqxx::result rows = execute_list_storage_usage_files(tx, user_id);

        std::vector<StorageUsageFileItem> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            const std::optional<StorageUsageFileItem> item = parse_storage_usage_file_row(pqxx::row(row));
            if (!item.has_value()) {
                throw std::runtime_error("storage_usage_row_invalid");
            }
            items.push_back(*item);
        }

        tx.commit();
        return StorageUsageInfo{.quota_bytes = *quota_bytes, .items = std::move(items)};
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("collect storage usage failed")
            .field("user_id", user_id)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("collect storage usage failed")
            .field("user_id", user_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<void, StorageError> StorageRepository::create_upload_session(const UploadSessionRecord& session,
                                                                           std::int64_t user_id) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());

        if (!is_user_scoped_storage_path(user_id, session.path)) {
            return std::unexpected(StorageError::ParentNotFound);
        }

        const auto parent_check = validate_parent_directory(tx, session.path, user_id);
        if (!parent_check.has_value()) {
            return std::unexpected(parent_check.error());
        }

        const std::optional<StorageNodeType> existing_type = find_node_type_for_update(tx, session.path);
        if (existing_type.has_value() && *existing_type == StorageNodeType::Directory) {
            return std::unexpected(StorageError::PathConflict);
        }
        if (has_descendant_nodes(tx, session.path)) {
            return std::unexpected(StorageError::PathConflict);
        }

        lock_storage_temp_path_sync(tx, session.temp_rel_path);
        const pqxx::result inserted = execute_create_upload_session(tx, session, now_s.count());
        if (inserted.empty()) {
            return std::unexpected(StorageError::InternalError);
        }
        tx.commit();
        return {};
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("create upload session failed")
            .field("upload_id", session.upload_id)
            .field("path", session.path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("create upload session failed")
            .field("upload_id", session.upload_id)
            .field("path", session.path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<UploadChunkAppendInfo, StorageError> StorageRepository::append_upload_chunk(
    std::string_view upload_id, std::int64_t user_id, std::int64_t chunk_index,
    const std::filesystem::path& storage_root_dir,
    const std::function<AppendTempChunkResult(const std::filesystem::path&, std::int64_t)>& append_chunk_file,
    const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    std::filesystem::path temp_abs_path;
    std::int64_t committed_size_bytes = 0;
    bool file_appended = false;
    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        const std::optional<UploadSessionChunkRow> session = find_upload_session_chunk_row_for_update(tx, upload_id);
        if (!session.has_value()) {
            return std::unexpected(StorageError::UploadNotFound);
        }
        if (!is_user_scoped_storage_path(user_id, session->path)) {
            return std::unexpected(StorageError::UploadNotFound);
        }
        if (session->next_chunk_index >= session->total_chunks) {
            return std::unexpected(StorageError::UploadAlreadyComplete);
        }
        if (session->next_chunk_index != chunk_index) {
            return std::unexpected(StorageError::InvalidChunkIndex);
        }
        if (!session->temp_size_bytes.has_value() || *session->temp_size_bytes < 0) {
            common::Logger::instance()
                .error("append upload chunk failed")
                .field("upload_id", upload_id)
                .field("chunk_index", chunk_index)
                .field("error", "temp_size_missing")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
        }

        committed_size_bytes = *session->temp_size_bytes;
        temp_abs_path = storage_root_dir / session->temp_rel_path;
        const AppendTempChunkResult append_result = append_chunk_file(temp_abs_path, committed_size_bytes);
        std::optional<StorageError> append_error;
        switch (append_result.status) {
            case AppendTempChunkStatus::Appended:
                break;
            case AppendTempChunkStatus::FileTooLarge:
                append_error = StorageError::FileTooLarge;
                break;
            case AppendTempChunkStatus::Failed:
                append_error = StorageError::InternalError;
                break;
        }
        if (append_error.has_value()) {
            restore_after_append_failure(upload_id, chunk_index, temp_abs_path, committed_size_bytes,
                                         restore_chunk_file);
            return std::unexpected(*append_error);
        }
        if (append_result.bytes_written < 0 ||
            append_result.bytes_written > (std::numeric_limits<std::int64_t>::max() - committed_size_bytes)) {
            restore_after_append_failure(upload_id, chunk_index, temp_abs_path, committed_size_bytes,
                                         restore_chunk_file);
            return std::unexpected(StorageError::InternalError);
        }

        file_appended = true;
        const std::int64_t new_size_bytes = committed_size_bytes + append_result.bytes_written;

        const pqxx::result updated = execute_advance_upload_session(tx, upload_id, now_s.count(), new_size_bytes);
        if (updated.empty()) {
            throw std::runtime_error("upload_session_update_missing");
        }
        const std::optional<std::int64_t> next_chunk_index_value = parse_next_chunk_index_row(updated.one_row());
        if (!next_chunk_index_value.has_value()) {
            throw std::runtime_error("upload_session_update_invalid");
        }

        tx.commit();
        file_appended = false;
        return UploadChunkAppendInfo{
            .total_chunks = session->total_chunks,
            .next_chunk_index = *next_chunk_index_value,
        };
    } catch (const pqxx::sql_error& e) {
        if (file_appended && !temp_abs_path.empty() && !restore_chunk_file(temp_abs_path, committed_size_bytes)) {
            common::Logger::instance()
                .warn("upload temp file restore failed")
                .field("upload_id", upload_id)
                .field("chunk_index", chunk_index)
                .field("path", temp_abs_path)
                .field("error", "restore_failed")
                .field("decision", "retry_on_next_chunk_upload");
        }
        common::Logger::instance()
            .error("append upload chunk failed")
            .field("upload_id", upload_id)
            .field("chunk_index", chunk_index)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        if (file_appended && !temp_abs_path.empty() && !restore_chunk_file(temp_abs_path, committed_size_bytes)) {
            common::Logger::instance()
                .warn("upload temp file restore failed")
                .field("upload_id", upload_id)
                .field("chunk_index", chunk_index)
                .field("path", temp_abs_path)
                .field("error", "restore_failed")
                .field("decision", "retry_on_next_chunk_upload");
        }
        common::Logger::instance()
            .error("append upload chunk failed")
            .field("upload_id", upload_id)
            .field("chunk_index", chunk_index)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<UploadCompletePrepareInfo, StorageError> StorageRepository::prepare_upload_complete(
    std::string_view upload_id, std::int64_t user_id) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const std::optional<UploadSessionChunkRow> session = find_upload_session_chunk_row(tx, upload_id);
        if (!session.has_value()) {
            return std::unexpected(StorageError::UploadNotFound);
        }
        if (!is_user_scoped_storage_path(user_id, session->path)) {
            return std::unexpected(StorageError::UploadNotFound);
        }
        if (session->next_chunk_index != session->total_chunks) {
            return std::unexpected(StorageError::UploadIncomplete);
        }
        if (!session->temp_size_bytes.has_value() || *session->temp_size_bytes < 0) {
            common::Logger::instance()
                .error("prepare upload complete failed")
                .field("upload_id", upload_id)
                .field("error", "temp_size_missing")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
        }

        return UploadCompletePrepareInfo{
            .scoped_path = session->path,
            .temp_rel_path = session->temp_rel_path,
            .temp_size_bytes = *session->temp_size_bytes,
        };
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("prepare upload complete failed")
            .field("upload_id", upload_id)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("prepare upload complete failed")
            .field("upload_id", upload_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<UploadCompleteFinalizeInfo, StorageError> StorageRepository::finalize_upload_complete(
    std::string_view upload_id, std::int64_t user_id, std::string_view sha256, std::int64_t size_bytes,
    std::string_view object_rel_path, const std::function<bool()>& publish_object_file) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_shared(tx);
        if (!publish_object_file()) {
            common::Logger::instance()
                .error("finalize upload complete failed")
                .field("upload_id", upload_id)
                .field("error", "object_file_publish_failed")
                .field("decision", "return_internal_error");
            return std::unexpected(StorageError::InternalError);
        }

        auto result =
            finalize_upload_complete_in_tx(tx, upload_id, user_id, sha256, size_bytes, object_rel_path, now_s);
        if (result.has_value()) {
            tx.commit();
        }
        return result;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("finalize upload complete failed")
            .field("upload_id", upload_id)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("finalize upload complete failed")
            .field("upload_id", upload_id)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

std::expected<DeleteNodeInfo, StorageError> StorageRepository::delete_node_record(std::int64_t user_id,
                                                                                  std::string_view scoped_path) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(StorageError::InternalError);
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        std::optional<std::string> cleanup_sha;

        pqxx::work tx(lease->connection());
        lock_storage_tree_sync(tx, user_id);
        ensure_user_root_directory_in_tx(tx, user_id, now_s.count());
        if (!is_user_scoped_storage_path(user_id, scoped_path)) {
            return std::unexpected(StorageError::PathNotFound);
        }

        const std::optional<NodeDeleteRow> existing = find_node_delete_row_for_update(tx, scoped_path);
        if (!existing.has_value()) {
            return std::unexpected(StorageError::PathNotFound);
        }

        if (existing->node_type == StorageNodeType::File) {
            if (!existing->sha256.has_value()) {
                throw std::runtime_error("file_node_sha_missing");
            }
            execute_delete_node(tx, scoped_path);
            const pqxx::result ref_rows =
                execute_decrement_storage_object_ref_count(tx, *existing->sha256, now_s.count());
            if (!ref_rows.empty()) {
                const std::optional<std::int64_t> ref_count = parse_storage_object_ref_count_row(ref_rows.one_row());
                if (ref_count.has_value() && *ref_count <= 0) {
                    cleanup_sha = existing->sha256;
                }
            }
            tx.commit();
            return DeleteNodeInfo{
                .node_type = StorageNodeType::File,
                .cleanup_sha = std::move(cleanup_sha),
            };
        }

        if (has_descendant_nodes(tx, scoped_path)) {
            return std::unexpected(StorageError::NonEmptyDirectory);
        }

        execute_delete_node(tx, scoped_path);
        tx.commit();
        return DeleteNodeInfo{
            .node_type = StorageNodeType::Directory,
            .cleanup_sha = std::nullopt,
        };
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("delete node record failed")
            .field("path", scoped_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("delete node record failed")
            .field("path", scoped_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(StorageError::InternalError);
    }
}

StoreDownloadTicketStatus StorageRepository::store_download_ticket(std::string_view ticket, std::int64_t user_id,
                                                                   std::string_view canonical_path,
                                                                   std::chrono::seconds expires_at_s) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return StoreDownloadTicketStatus::InternalError;
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        pqxx::work tx(lease->connection());
        const pqxx::result inserted =
            execute_store_download_ticket(tx, ticket, user_id, canonical_path, now_s.count(), expires_at_s.count());
        if (inserted.empty()) {
            tx.commit();
            return StoreDownloadTicketStatus::Duplicate;
        }
        tx.commit();
        return StoreDownloadTicketStatus::Stored;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("store download ticket failed")
            .field("user_id", user_id)
            .field("path", canonical_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return StoreDownloadTicketStatus::InternalError;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("store download ticket failed")
            .field("user_id", user_id)
            .field("path", canonical_path)
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return StoreDownloadTicketStatus::InternalError;
    }
}

std::expected<DownloadTicketInfo, FindDownloadTicketError> StorageRepository::find_download_ticket(
    std::string_view ticket) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(FindDownloadTicketError::InternalError);
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const std::optional<DownloadTicketRow> row = find_download_ticket_row(tx, ticket);
        if (!row.has_value()) {
            return std::unexpected(FindDownloadTicketError::NotFound);
        }
        if (row->user_id <= 0 || row->canonical_path.empty() || row->canonical_path == "/") {
            common::Logger::instance()
                .error("find download ticket failed")
                .field("error", "download_ticket_row_invalid")
                .field("decision", "return_internal_error");
            return std::unexpected(FindDownloadTicketError::InternalError);
        }

        return DownloadTicketInfo{
            .user_id = row->user_id,
            .canonical_path = row->canonical_path,
            .expires_at_s = row->expires_at_s,
        };
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("find download ticket failed")
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(FindDownloadTicketError::InternalError);
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("find download ticket failed")
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return std::unexpected(FindDownloadTicketError::InternalError);
    }
}

StorageGcSnapshot StorageRepository::collect_storage_gc_snapshot(std::chrono::seconds upload_session_ttl) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return {.status = StorageGcSnapshotStatus::InternalError,
                .expired_sessions = {},
                .active_temp_rel_paths = {},
                .object_rel_paths_in_db = {},
                .orphan_objects = {},
                .expired_download_ticket_count = 0};
    }

    try {
        const std::chrono::seconds now_s = common::now_epoch_seconds();
        const std::chrono::seconds expired_before_s = now_s - upload_session_ttl;
        StorageGcSnapshot snapshot;
        snapshot.status = StorageGcSnapshotStatus::Ready;

        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);

        const pqxx::result expired_rows = execute_find_expired_upload_sessions(tx, expired_before_s.count());
        snapshot.expired_sessions.reserve(expired_rows.size());
        for (const auto& row : expired_rows) {
            const std::optional<GcExpiredUploadSession> session = parse_gc_expired_upload_session_row(pqxx::row(row));
            if (!session.has_value()) {
                throw std::runtime_error("gc_expired_upload_session_row_invalid");
            }
            snapshot.expired_sessions.push_back(*session);
        }
        execute_delete_expired_upload_sessions(tx, expired_before_s.count());

        const pqxx::result expired_ticket_rows = execute_delete_expired_download_tickets(tx, now_s.count());
        snapshot.expired_download_ticket_count = static_cast<std::int64_t>(expired_ticket_rows.size());

        const pqxx::result active_temp_rows = execute_list_active_temp_rel_paths(tx);
        snapshot.active_temp_rel_paths.reserve(active_temp_rows.size());
        for (const auto& row : active_temp_rows) {
            const std::optional<std::string> path = parse_string_value_row(pqxx::row(row));
            if (!path.has_value()) {
                throw std::runtime_error("active_temp_path_row_invalid");
            }
            snapshot.active_temp_rel_paths.push_back(*path);
        }

        execute_sync_storage_object_ref_counts(tx, now_s.count());
        execute_reset_orphan_storage_object_ref_counts(tx, now_s.count());

        const pqxx::result object_path_rows = execute_list_object_rel_paths(tx);
        snapshot.object_rel_paths_in_db.reserve(object_path_rows.size());
        for (const auto& row : object_path_rows) {
            const std::optional<std::string> path = parse_string_value_row(pqxx::row(row));
            if (!path.has_value()) {
                throw std::runtime_error("object_rel_path_row_invalid");
            }
            snapshot.object_rel_paths_in_db.push_back(*path);
        }

        const pqxx::result orphan_rows = execute_list_orphan_storage_objects(tx);
        snapshot.orphan_objects.reserve(orphan_rows.size());
        for (const auto& row : orphan_rows) {
            const std::optional<GcOrphanObject> orphan = parse_gc_orphan_object_row(pqxx::row(row));
            if (!orphan.has_value()) {
                throw std::runtime_error("orphan_storage_object_row_invalid");
            }
            snapshot.orphan_objects.push_back(*orphan);
        }

        tx.commit();
        return snapshot;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("collect storage gc snapshot failed")
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = StorageGcSnapshotStatus::InternalError,
                .expired_sessions = {},
                .active_temp_rel_paths = {},
                .object_rel_paths_in_db = {},
                .orphan_objects = {},
                .expired_download_ticket_count = 0};
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("collect storage gc snapshot failed")
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return {.status = StorageGcSnapshotStatus::InternalError,
                .expired_sessions = {},
                .active_temp_rel_paths = {},
                .object_rel_paths_in_db = {},
                .orphan_objects = {},
                .expired_download_ticket_count = 0};
    }
}

CleanupStatus StorageRepository::cleanup_unreferenced_object(
    std::string_view sha256, const std::function<bool(std::string_view)>& delete_object_file) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return CleanupStatus::InternalError;
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);

        const std::optional<UnreferencedObjectCleanupRow> row =
            find_cleanup_candidate_object_row_for_update(tx, sha256);
        if (!row.has_value()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }
        if (row->ref_count > 0 || row->has_file_node_reference) {
            tx.commit();
            return CleanupStatus::Skipped;
        }
        if (!delete_object_file(row->object_rel_path)) {
            common::Logger::instance()
                .warn("cleanup object file failed")
                .field("sha256", sha256)
                .field("path", row->object_rel_path)
                .field("decision", "keep_orphan_row_for_gc");
            tx.commit();
            return CleanupStatus::Skipped;
        }

        execute_delete_cleanup_candidate_object(tx, sha256);
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error("cleanup unreferenced object failed")
            .field("sha256", sha256)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "keep_orphan_row_for_gc");
        return CleanupStatus::InternalError;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("cleanup unreferenced object failed")
            .field("sha256", sha256)
            .field("error", e.what())
            .field("decision", "keep_orphan_row_for_gc");
        return CleanupStatus::InternalError;
    }
}

CleanupStatus StorageRepository::cleanup_file_only_object(
    std::string_view object_rel_path, const std::filesystem::path& object_abs_path,
    const std::function<bool(const std::filesystem::path&)>& delete_object_file) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return CleanupStatus::InternalError;
    }

    try {
        if (object_rel_path.empty() || object_rel_path.starts_with("../") || object_rel_path == "..") {
            common::Logger::instance()
                .warn("cleanup file-only orphan object failed")
                .field("path", object_abs_path)
                .field("error", "invalid_object_path")
                .field("decision", "keep_orphan");
            return CleanupStatus::InternalError;
        }

        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);
        const pqxx::result referenced_rows = execute_find_storage_object_by_rel_path(tx, object_rel_path);
        if (!referenced_rows.empty()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        if (!delete_object_file(object_abs_path)) {
            common::Logger::instance()
                .warn("cleanup file-only orphan object failed")
                .field("path", object_abs_path)
                .field("error", "object_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return CleanupStatus::Skipped;
        }
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .warn("cleanup file-only orphan object failed")
            .field("path", object_abs_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn("cleanup file-only orphan object failed")
            .field("path", object_abs_path)
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    }
}

CleanupStatus StorageRepository::cleanup_temp_file(
    std::string_view temp_rel_path, const std::filesystem::path& temp_abs_path,
    const std::function<bool(const std::filesystem::path&)>& delete_temp_file) {
    try {
        if (temp_rel_path.empty() || temp_rel_path.starts_with("../") || temp_rel_path == ".." ||
            !temp_rel_path.starts_with("temp/")) {
            common::Logger::instance()
                .warn("cleanup temp file failed")
                .field("path", temp_abs_path)
                .field("error", "invalid_temp_path")
                .field("decision", "keep_orphan");
            return CleanupStatus::InternalError;
        }

        const auto lease = database_pool_->acquire_lease();
        if (!lease.has_value()) {
            return CleanupStatus::InternalError;
        }

        pqxx::work tx(lease->connection());
        lock_storage_temp_path_sync(tx, temp_rel_path);
        const pqxx::result referenced_rows = execute_find_upload_session_by_temp_rel_path(tx, temp_rel_path);
        if (!referenced_rows.empty()) {
            tx.commit();
            return CleanupStatus::Skipped;
        }

        if (!delete_temp_file(temp_abs_path)) {
            common::Logger::instance()
                .warn("cleanup temp file failed")
                .field("path", temp_abs_path)
                .field("error", "temp_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return CleanupStatus::Skipped;
        }
        tx.commit();
        return CleanupStatus::Cleaned;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .warn("cleanup temp file failed")
            .field("path", temp_abs_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn("cleanup temp file failed")
            .field("path", temp_abs_path)
            .field("error", e.what())
            .field("decision", "keep_orphan");
        return CleanupStatus::InternalError;
    }
}

void StorageRepository::cleanup_upload_failure_object(
    const std::filesystem::path& object_abs_path, std::string_view upload_id, std::string_view sha256,
    const std::function<bool(const std::filesystem::path&)>& delete_object_file) {
    if (sha256.empty() || object_abs_path.empty()) {
        return;
    }

    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return;
    }

    try {
        pqxx::work tx(lease->connection());
        lock_storage_object_gc_sync_exclusive(tx);
        const pqxx::result referenced_rows = execute_find_upload_failure_object_reference(tx, sha256);
        if (!referenced_rows.empty()) {
            tx.commit();
            return;
        }

        if (!delete_object_file(object_abs_path)) {
            common::Logger::instance()
                .warn("upload complete rollback object cleanup failed")
                .field("upload_id", upload_id)
                .field("sha256", sha256)
                .field("path", object_abs_path)
                .field("error", "object_file_delete_failed")
                .field("decision", "keep_orphan");
            tx.commit();
            return;
        }
        tx.commit();
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .warn("upload complete rollback object cleanup failed")
            .field("upload_id", upload_id)
            .field("sha256", sha256)
            .field("path", object_abs_path)
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "keep_orphan");
    } catch (const std::exception& e) {
        common::Logger::instance()
            .warn("upload complete rollback object cleanup failed")
            .field("upload_id", upload_id)
            .field("sha256", sha256)
            .field("path", object_abs_path)
            .field("error", e.what())
            .field("decision", "keep_orphan");
    }
}

}  // namespace nebula::storage
