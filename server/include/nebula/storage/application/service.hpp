#ifndef NEBULA_STORAGE_APPLICATION_SERVICE_HPP
#define NEBULA_STORAGE_APPLICATION_SERVICE_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/storage/domain/error.hpp"
#include "nebula/storage/domain/types.hpp"
#include "nebula/storage/infra/object_store.hpp"
#include "nebula/storage/repository/repository.hpp"

namespace nebula::storage {

struct InitUploadCmd {
    std::string canonical_path;
    std::int64_t total_chunks = 0;
};

struct UsageBreakdownItem {
    std::string file_type;
    std::int64_t size_bytes = 0;
    std::int64_t file_count = 0;
};

struct InitUploadResult {
    std::string upload_id;
    std::string path;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
};

struct AppendChunkResult {
    std::string upload_id;
    std::int64_t chunk_index = 0;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
};

struct CompleteUploadResult {
    std::string upload_id;
    std::string path;
    std::string sha256;
    std::int64_t size_bytes = 0;
};

struct CreateDirectoryServiceResult {
    std::string path;
};

struct ListDirectoryResult {
    std::string path;
    std::vector<TreeItem> items;
};

struct DeleteNodeServiceResult {
    std::string path;
    StorageNodeType node_type = StorageNodeType::File;
};

struct RecentFilesServiceResult {
    std::int64_t limit = 0;
    std::vector<RecentFileItem> items;
};

struct StorageUsageSummaryResult {
    std::int64_t total_bytes = 0;
    std::int64_t used_bytes = 0;
    std::int64_t available_bytes = 0;
    std::int64_t used_percent = 0;
    std::int64_t max_chunk_bytes = 0;
    std::int64_t max_file_bytes = 0;
    std::vector<UsageBreakdownItem> breakdown;
};

struct IssueTicketResult {
    std::string path;
    std::string ticket;
    std::int64_t expires_at_s = 0;
};

struct PrepareDownloadResult {
    std::string canonical_path;
    std::string sha256;
    std::string body;
};

struct GcResult {
    std::int64_t expired_upload_sessions = 0;
    std::int64_t expired_download_tickets = 0;
    std::int64_t cleaned_temp_files = 0;
    std::int64_t unreferenced_objects = 0;
    std::int64_t cleaned_unreferenced_objects = 0;
    std::int64_t file_only_objects = 0;
    std::int64_t cleaned_file_only_objects = 0;
    std::int64_t orphan_temp_files = 0;
    std::int64_t cleaned_orphan_temp_files = 0;
};

class StorageService {
public:
    StorageService(std::shared_ptr<StorageRepository> repository, std::shared_ptr<ObjectStore> object_store,
                   StorageRuntimeConfig config);

    [[nodiscard]] std::expected<InitUploadResult, StorageError> init_upload(std::int64_t user_id,
                                                                            const InitUploadCmd& cmd);
    [[nodiscard]] std::expected<AppendChunkResult, StorageError> append_chunk(std::int64_t user_id,
                                                                              std::string_view upload_id,
                                                                              std::int64_t chunk_index,
                                                                              std::string_view chunk_data);
    [[nodiscard]] std::expected<CompleteUploadResult, StorageError> complete_upload(std::int64_t user_id,
                                                                                    std::string_view upload_id);

    [[nodiscard]] std::expected<CreateDirectoryServiceResult, StorageError> create_directory(
        std::int64_t user_id, std::string_view canonical_path);
    [[nodiscard]] std::expected<ListDirectoryResult, StorageError> list_directory(std::int64_t user_id,
                                                                                  std::string_view canonical_path,
                                                                                  const DirectoryListOptions& options);
    [[nodiscard]] std::expected<DeleteNodeServiceResult, StorageError> delete_node(std::int64_t user_id,
                                                                                   std::string_view canonical_path);

    [[nodiscard]] std::expected<RecentFilesServiceResult, StorageError> list_recent(std::int64_t user_id,
                                                                                    std::int64_t limit);
    [[nodiscard]] std::expected<StorageUsageSummaryResult, StorageError> usage(std::int64_t user_id);

    [[nodiscard]] std::expected<IssueTicketResult, StorageError> issue_download_ticket(std::int64_t user_id,
                                                                                       std::string_view canonical_path);
    [[nodiscard]] std::expected<PrepareDownloadResult, StorageError> prepare_download(std::string_view ticket);

    [[nodiscard]] std::expected<GcResult, StorageError> run_gc();

private:
    std::shared_ptr<StorageRepository> repository_;
    std::shared_ptr<ObjectStore> object_store_;
    StorageRuntimeConfig config_;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_APPLICATION_SERVICE_HPP
