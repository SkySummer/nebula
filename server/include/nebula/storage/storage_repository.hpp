#ifndef NEBULA_STORAGE_STORAGE_REPOSITORY_HPP
#define NEBULA_STORAGE_STORAGE_REPOSITORY_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/storage/storage_types.hpp"

namespace nebula::storage {

constexpr std::int64_t kStorageObjectGcAdvisoryLockKey = 0x4e4542554c41;

enum class FileLookupStatus : std::uint8_t {
    Found,
    NotFound,
    Directory,
    InternalError,
};

struct FileLookupResult {
    FileLookupStatus status = FileLookupStatus::InternalError;
    FileNodeRecord node;
};

enum class UploadSessionCreateStatus : std::uint8_t {
    Created,
    ParentNotFound,
    ParentNotDirectory,
    PathConflict,
    InternalError,
};

struct UploadSessionCreateResult {
    UploadSessionCreateStatus status = UploadSessionCreateStatus::InternalError;
};

enum class UploadChunkAppendStatus : std::uint8_t {
    Advanced,
    NotFound,
    AlreadyComplete,
    InvalidChunkIndex,
    FileTooLarge,
    InternalError,
};

struct UploadChunkAppendResult {
    UploadChunkAppendStatus status = UploadChunkAppendStatus::InternalError;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
};

enum class UploadChunkFileAppendStatus : std::uint8_t {
    Appended,
    FileTooLarge,
    Failed,
};

struct UploadChunkFileAppendResult {
    UploadChunkFileAppendStatus status = UploadChunkFileAppendStatus::Failed;
    std::int64_t bytes_written = 0;
};

enum class UploadCompletePrepareStatus : std::uint8_t {
    Ready,
    NotFound,
    Incomplete,
    InternalError,
};

struct UploadCompletePrepareResult {
    UploadCompletePrepareStatus status = UploadCompletePrepareStatus::InternalError;
    std::string scoped_path;
    std::string temp_rel_path;
    std::int64_t temp_size_bytes = 0;
};

enum class UploadCompleteFinalizeStatus : std::uint8_t {
    Completed,
    NotFound,
    Incomplete,
    ParentNotFound,
    ParentNotDirectory,
    PathConflict,
    InternalError,
};

struct UploadCompleteFinalizeResult {
    UploadCompleteFinalizeStatus status = UploadCompleteFinalizeStatus::InternalError;
    std::string scoped_path;
    std::optional<std::string> cleanup_old_sha;
};

enum class DeleteNodeStatus : std::uint8_t {
    FileDeleted,
    DirectoryDeleted,
    NotFound,
    NonEmptyDirectory,
    InternalError,
};

struct DeleteNodeResult {
    DeleteNodeStatus status = DeleteNodeStatus::InternalError;
    std::optional<std::string> cleanup_sha;
};

enum class CreateDirectoryStatus : std::uint8_t {
    Created,
    AlreadyExists,
    ParentNotFound,
    ParentNotDirectory,
    PathConflict,
    InternalError,
};

struct CreateDirectoryResult {
    CreateDirectoryStatus status = CreateDirectoryStatus::InternalError;
};

enum class DirectoryListStatus : std::uint8_t {
    Listed,
    NotFound,
    NotDirectory,
    InternalError,
};

struct DirectoryListResult {
    DirectoryListStatus status = DirectoryListStatus::InternalError;
    std::vector<TreeItem> items;
};

struct GcExpiredUploadSession {
    std::string upload_id;
    std::string temp_rel_path;
};

struct GcOrphanObject {
    std::string sha256;
    std::string object_rel_path;
};

enum class StorageGcSnapshotStatus : std::uint8_t {
    Ready,
    InternalError,
};

struct StorageGcSnapshot {
    StorageGcSnapshotStatus status = StorageGcSnapshotStatus::InternalError;
    std::vector<GcExpiredUploadSession> expired_sessions;
    std::vector<std::string> active_temp_rel_paths;
    std::vector<std::string> object_rel_paths_in_db;
    std::vector<GcOrphanObject> orphan_objects;
};

enum class CleanupStatus : std::uint8_t {
    Cleaned,
    Skipped,
    InternalError,
};

[[nodiscard]] bool check_storage_schema_ready();

[[nodiscard]] bool ensure_user_root_directory(std::int64_t user_id, std::int64_t now_s);

[[nodiscard]] FileLookupResult find_file_node(std::string_view path);

[[nodiscard]] CreateDirectoryResult create_directory_node(std::int64_t user_id, std::string_view scoped_path,
                                                          std::int64_t now_s);

[[nodiscard]] DirectoryListResult list_directory_children(std::int64_t user_id, std::string_view scoped_path,
                                                          std::int64_t now_s);

[[nodiscard]] UploadSessionCreateResult create_upload_session(const UploadSessionRecord& session, std::int64_t user_id,
                                                              std::int64_t now_s);

[[nodiscard]] UploadChunkAppendResult append_upload_chunk(
    std::string_view upload_id, std::int64_t user_id, std::int64_t chunk_index, std::int64_t now_s,
    const std::filesystem::path& storage_root_dir,
    const std::function<UploadChunkFileAppendResult(const std::filesystem::path&, std::int64_t)>& append_chunk_file,
    const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file);

[[nodiscard]] UploadCompletePrepareResult prepare_upload_complete(std::string_view upload_id, std::int64_t user_id);

[[nodiscard]] UploadCompleteFinalizeResult finalize_upload_complete(std::string_view upload_id, std::int64_t user_id,
                                                                    std::string_view sha256, std::int64_t size_bytes,
                                                                    std::string_view object_rel_path,
                                                                    std::int64_t now_s,
                                                                    const std::function<bool()>& publish_object_file);

[[nodiscard]] DeleteNodeResult delete_node_record(std::int64_t user_id, std::string_view scoped_path,
                                                  std::int64_t now_s);

[[nodiscard]] StorageGcSnapshot collect_storage_gc_snapshot(std::int64_t expired_before_s, std::int64_t now_s);

CleanupStatus cleanup_unreferenced_object(const StorageRouteConfig& config, std::string_view sha256);

CleanupStatus cleanup_file_only_object(const StorageRouteConfig& config, const std::filesystem::path& object_abs_path);

CleanupStatus cleanup_temp_file(const StorageRouteConfig& config, const std::filesystem::path& temp_abs_path);

void cleanup_upload_failure_object(const StorageRouteConfig& config, std::string_view upload_id,
                                   std::string_view sha256, const std::filesystem::path& object_abs_path);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_STORAGE_REPOSITORY_HPP
