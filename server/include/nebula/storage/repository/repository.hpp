#ifndef NEBULA_STORAGE_REPOSITORY_REPOSITORY_HPP
#define NEBULA_STORAGE_REPOSITORY_REPOSITORY_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>

#include "nebula/database/connection_pool.hpp"
#include "nebula/storage/domain/error.hpp"
#include "nebula/storage/infra/object_store.hpp"
#include "nebula/storage/repository/types.hpp"

namespace nebula::storage {

class StorageRepository {
public:
    explicit StorageRepository(std::shared_ptr<database::ConnectionPool> database_pool);

    [[nodiscard]] bool check_schema_ready();

    [[nodiscard]] bool ensure_user_root_directory(std::int64_t user_id);

    [[nodiscard]] std::expected<FileNodeRecord, StorageError> find_file_node(std::string_view path);

    [[nodiscard]] std::expected<void, StorageError> create_directory_node(std::int64_t user_id,
                                                                          std::string_view scoped_path);

    [[nodiscard]] std::expected<std::vector<TreeItem>, StorageError> list_directory_children(
        std::int64_t user_id, std::string_view scoped_path, const DirectoryListOptions& options);

    [[nodiscard]] std::expected<std::vector<RecentFileItem>, StorageError> list_recent_files(std::int64_t user_id,
                                                                                             std::int64_t limit);

    [[nodiscard]] std::expected<StorageUsageInfo, StorageError> collect_storage_usage(std::int64_t user_id);

    [[nodiscard]] std::expected<void, StorageError> create_upload_session(const UploadSessionRecord& session,
                                                                          std::int64_t user_id);

    [[nodiscard]] std::expected<UploadChunkAppendInfo, StorageError> append_upload_chunk(
        std::string_view upload_id, std::int64_t user_id, std::int64_t chunk_index,
        const std::filesystem::path& storage_root_dir,
        const std::function<AppendTempChunkResult(const std::filesystem::path&, std::int64_t)>& append_chunk_file,
        const std::function<bool(const std::filesystem::path&, std::int64_t)>& restore_chunk_file);

    [[nodiscard]] std::expected<UploadCompletePrepareInfo, StorageError> prepare_upload_complete(
        std::string_view upload_id, std::int64_t user_id);

    [[nodiscard]] std::expected<UploadCompleteFinalizeInfo, StorageError> finalize_upload_complete(
        std::string_view upload_id, std::int64_t user_id, std::string_view sha256, std::int64_t size_bytes,
        std::string_view object_rel_path, const std::function<bool()>& publish_object_file);

    [[nodiscard]] std::expected<DeleteNodeInfo, StorageError> delete_node_record(std::int64_t user_id,
                                                                                 std::string_view scoped_path);

    [[nodiscard]] StorageGcSnapshot collect_storage_gc_snapshot(std::chrono::seconds upload_session_ttl);

    [[nodiscard]] StoreDownloadTicketStatus store_download_ticket(std::string_view ticket, std::int64_t user_id,
                                                                  std::string_view canonical_path,
                                                                  std::chrono::seconds expires_at_s);

    [[nodiscard]] std::expected<DownloadTicketInfo, FindDownloadTicketError> find_download_ticket(
        std::string_view ticket);

    CleanupStatus cleanup_unreferenced_object(std::string_view sha256,
                                              const std::function<bool(std::string_view)>& delete_object_file);

    CleanupStatus cleanup_file_only_object(std::string_view object_rel_path,
                                           const std::filesystem::path& object_abs_path,
                                           const std::function<bool(const std::filesystem::path&)>& delete_object_file);

    CleanupStatus cleanup_temp_file(std::string_view temp_rel_path, const std::filesystem::path& temp_abs_path,
                                    const std::function<bool(const std::filesystem::path&)>& delete_temp_file);

    void cleanup_upload_failure_object(const std::filesystem::path& object_abs_path, std::string_view upload_id,
                                       std::string_view sha256,
                                       const std::function<bool(const std::filesystem::path&)>& delete_object_file);

private:
    std::shared_ptr<database::ConnectionPool> database_pool_;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_REPOSITORY_HPP
