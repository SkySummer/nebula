#ifndef NEBULA_STORAGE_REPOSITORY_TYPES_HPP
#define NEBULA_STORAGE_REPOSITORY_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nebula/storage/domain/types.hpp"

namespace nebula::storage {

constexpr std::int64_t kGlobalStorageObjectGcAdvisoryLockKey = 0x261a'40d5'38b2'b506;

struct UploadChunkAppendInfo {
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
};

struct UploadCompletePrepareInfo {
    std::string scoped_path;
    std::string temp_rel_path;
    std::int64_t temp_size_bytes = 0;
};

struct UploadCompleteFinalizeInfo {
    std::string scoped_path;
    std::optional<std::string> cleanup_old_sha;
};

struct DeleteNodeInfo {
    StorageNodeType node_type = StorageNodeType::File;
    std::optional<std::string> cleanup_sha;
};

struct StorageUsageInfo {
    std::int64_t quota_bytes = 0;
    std::vector<StorageUsageFileItem> items;
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
    std::int64_t expired_download_ticket_count = 0;
};

enum class CleanupStatus : std::uint8_t {
    Cleaned,
    Skipped,
    InternalError,
};

enum class StoreDownloadTicketStatus : std::uint8_t {
    Stored,
    Duplicate,
    InternalError,
};

enum class FindDownloadTicketError : std::uint8_t {
    NotFound,
    InternalError,
};

struct DownloadTicketInfo {
    std::int64_t user_id = 0;
    std::string canonical_path;
    std::int64_t expires_at_s = 0;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_TYPES_HPP
