#include "nebula/storage/application/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula/common/base/arithmetic.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/security/crypto.hpp"
#include "nebula/storage/domain/file_types.hpp"
#include "nebula/storage/domain/path.hpp"
#include "nebula/storage/infra/download_ticket.hpp"

namespace nebula::storage {

namespace {

constexpr std::chrono::seconds kFileOnlyOrphanMinAge{30};
constexpr std::int64_t kPercentScale = 100;
constexpr std::int64_t kMaxRecentLimit = 64;
constexpr int kMaxStoreDownloadTicketAttempts = 4;

template <typename Value>
[[nodiscard]] std::expected<Value, StorageError> make_error_result(StorageError error) {
    return std::unexpected(error);
}

[[nodiscard]] std::string scoped_storage_path(std::int64_t user_id, std::string_view canonical_path) {
    if (canonical_path == "/") {
        return std::format("/users/{}", user_id);
    }
    return std::format("/users/{}{}", user_id, canonical_path);
}

[[nodiscard]] std::string to_public_storage_path(std::int64_t user_id, std::string_view scoped_path) {
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

[[nodiscard]] std::int64_t rounded_ratio_percent(std::int64_t numerator, std::int64_t denominator, bool& saturated) {
    if (denominator <= 0 || numerator <= 0) {
        return 0;
    }

    const std::int64_t quotient = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;
    const std::int64_t scaled_quotient = common::saturating_mul(quotient, kPercentScale, saturated);

    std::int64_t scaled_remainder = 0;
    std::int64_t rolling = denominator / 2;
    for (std::int64_t idx = 0; idx < kPercentScale; ++idx) {
        if (remainder >= denominator - rolling) {
            rolling = remainder - denominator + rolling;
            ++scaled_remainder;
        } else {
            rolling += remainder;
        }
    }

    return common::saturating_add(scaled_quotient, scaled_remainder, saturated);
}

[[nodiscard]] std::string build_object_rel_path(std::string_view sha256) {
    return std::format("objects/{}/{}/{}", sha256.substr(0, 2), sha256.substr(2, 2), sha256);
}

[[nodiscard]] std::expected<FileNodeRecord, StorageError> resolve_download_target(StorageRepository& repository,
                                                                                  const StorageRuntimeConfig& config,
                                                                                  std::int64_t user_id,
                                                                                  std::string_view canonical_path) {
    auto lookup = repository.find_file_node(scoped_storage_path(user_id, canonical_path));
    if (!lookup.has_value()) {
        return std::unexpected(lookup.error());
    }

    if (lookup->size_bytes < 0 || lookup->size_bytes > config.max_file_bytes) {
        return std::unexpected(StorageError::FileTooLarge);
    }

    return *lookup;
}

}  // namespace

std::string_view to_string(StorageError error) noexcept {
    switch (error) {
        case StorageError::InvalidPath:
            return "invalid_path";
        case StorageError::InvalidChunkIndex:
            return "invalid_chunk_index";
        case StorageError::InvalidLimit:
            return "invalid_limit";
        case StorageError::RootDeleteNotAllowed:
            return "root_delete_not_allowed";
        case StorageError::ParentNotFound:
            return "parent_not_found";
        case StorageError::ParentNotDirectory:
            return "parent_not_directory";
        case StorageError::PathConflict:
            return "path_conflict";
        case StorageError::PathNotFound:
            return "path_not_found";
        case StorageError::NotDirectory:
            return "not_directory";
        case StorageError::NotFile:
            return "not_file";
        case StorageError::DirectoryAlreadyExists:
            return "directory_already_exists";
        case StorageError::UploadNotFound:
            return "upload_not_found";
        case StorageError::UploadAlreadyComplete:
            return "upload_already_complete";
        case StorageError::UploadIncomplete:
            return "upload_incomplete";
        case StorageError::FileTooLarge:
            return "file_too_large";
        case StorageError::StorageQuotaExceeded:
            return "storage_quota_exceeded";
        case StorageError::NonEmptyDirectory:
            return "non_empty_directory";
        case StorageError::DownloadTicketInvalid:
            return "download_ticket_invalid";
        case StorageError::DownloadTicketExpired:
            return "download_ticket_expired";
        case StorageError::InternalError:
            return "internal_error";
    }
    std::unreachable();
}

StorageService::StorageService(std::shared_ptr<StorageRepository> repository, std::shared_ptr<ObjectStore> object_store,
                               StorageRuntimeConfig config)
    : repository_(std::move(repository)), object_store_(std::move(object_store)), config_(std::move(config)) {
    if (repository_ == nullptr) {
        throw std::invalid_argument("repository_missing");
    }
    if (object_store_ == nullptr) {
        throw std::invalid_argument("object_store_missing");
    }
    if (config_.root_dir.empty()) {
        throw std::invalid_argument("root_dir_missing");
    }
    if (config_.upload_session_ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("upload_session_ttl_invalid");
    }
    if (config_.download_ticket_ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("download_ticket_ttl_invalid");
    }
    if (config_.max_body_bytes == 0U) {
        throw std::invalid_argument("max_body_bytes_invalid");
    }
    if (config_.max_file_bytes <= 0) {
        throw std::invalid_argument("max_file_bytes_invalid");
    }
}

std::expected<InitUploadResult, StorageError> StorageService::init_upload(std::int64_t user_id,
                                                                          const InitUploadCmd& cmd) {
    if (user_id <= 0) {
        return make_error_result<InitUploadResult>(StorageError::InternalError);
    }
    if (!validate_canonical_path(cmd.canonical_path) || cmd.canonical_path == "/" || cmd.total_chunks <= 0) {
        return make_error_result<InitUploadResult>(StorageError::InvalidPath);
    }

    const std::optional<std::string> upload_id = common::generate_random_hex_token_128();
    if (!upload_id.has_value()) {
        return make_error_result<InitUploadResult>(StorageError::InternalError);
    }

    const std::string temp_rel_path = std::format("temp/{}.part", *upload_id);
    const std::filesystem::path temp_abs_path = config_.root_dir / temp_rel_path;
    if (!ObjectStore::create_empty_temp_file(temp_abs_path)) {
        return make_error_result<InitUploadResult>(StorageError::InternalError);
    }

    const UploadSessionRecord session{
        .upload_id = *upload_id,
        .path = scoped_storage_path(user_id, cmd.canonical_path),
        .temp_rel_path = temp_rel_path,
        .total_chunks = cmd.total_chunks,
        .next_chunk_index = 0,
    };
    const auto create_result = repository_->create_upload_session(session, user_id);
    if (!create_result.has_value()) {
        [[maybe_unused]] const bool temp_deleted = object_store_->delete_temp_path(temp_abs_path);
        return make_error_result<InitUploadResult>(create_result.error());
    }

    return InitUploadResult{
        .upload_id = *upload_id,
        .path = cmd.canonical_path,
        .total_chunks = cmd.total_chunks,
        .next_chunk_index = 0,
    };
}

std::expected<AppendChunkResult, StorageError> StorageService::append_chunk(std::int64_t user_id,
                                                                            std::string_view upload_id,
                                                                            std::int64_t chunk_index,
                                                                            std::string_view chunk_data) {
    if (user_id <= 0) {
        return make_error_result<AppendChunkResult>(StorageError::InternalError);
    }
    if (upload_id.empty() || chunk_index < 0) {
        return make_error_result<AppendChunkResult>(StorageError::InvalidChunkIndex);
    }

    auto append_result = repository_->append_upload_chunk(
        upload_id, user_id, chunk_index, config_.root_dir,
        [this, chunk_data](const std::filesystem::path& temp_abs_path, std::int64_t committed_size_bytes) {
            return object_store_->append_temp_chunk(temp_abs_path, chunk_data, committed_size_bytes);
        },
        [](const std::filesystem::path& temp_abs_path, std::int64_t committed_size_bytes) {
            return ObjectStore::restore_temp_size(temp_abs_path, committed_size_bytes);
        });
    if (!append_result.has_value()) {
        return std::unexpected(append_result.error());
    }

    return AppendChunkResult{
        .upload_id = std::string(upload_id),
        .chunk_index = chunk_index,
        .total_chunks = append_result->total_chunks,
        .next_chunk_index = append_result->next_chunk_index,
    };
}

std::expected<CompleteUploadResult, StorageError> StorageService::complete_upload(std::int64_t user_id,
                                                                                  std::string_view upload_id) {
    if (user_id <= 0 || upload_id.empty()) {
        return make_error_result<CompleteUploadResult>(StorageError::InternalError);
    }

    auto prepare_result = repository_->prepare_upload_complete(upload_id, user_id);
    if (!prepare_result.has_value()) {
        return make_error_result<CompleteUploadResult>(prepare_result.error());
    }

    const std::filesystem::path temp_abs_path = config_.root_dir / prepare_result->temp_rel_path;
    if (prepare_result->temp_size_bytes > config_.max_file_bytes) {
        return make_error_result<CompleteUploadResult>(StorageError::FileTooLarge);
    }
    if (!ObjectStore::restore_temp_size(temp_abs_path, prepare_result->temp_size_bytes)) {
        return make_error_result<CompleteUploadResult>(StorageError::InternalError);
    }

    const std::optional<HashAndSize> hash_and_size = ObjectStore::hash_file(temp_abs_path);
    if (!hash_and_size.has_value()) {
        return make_error_result<CompleteUploadResult>(StorageError::InternalError);
    }
    if (hash_and_size->sha256_hex.size() < 4U) {
        return make_error_result<CompleteUploadResult>(StorageError::InternalError);
    }

    const std::string object_rel_path = build_object_rel_path(hash_and_size->sha256_hex);
    const std::filesystem::path object_abs_path = config_.root_dir / object_rel_path;
    PublishObjectStatus publish_status = PublishObjectStatus::Failed;
    auto finalize_result = repository_->finalize_upload_complete(
        upload_id, user_id, hash_and_size->sha256_hex, hash_and_size->size_bytes, object_rel_path, [&]() {
            publish_status = ObjectStore::publish_object(temp_abs_path, object_abs_path, hash_and_size->sha256_hex,
                                                         hash_and_size->size_bytes);
            return publish_status != PublishObjectStatus::Failed;
        });
    if (!finalize_result.has_value() && publish_status == PublishObjectStatus::Created) {
        repository_->cleanup_upload_failure_object(
            object_abs_path, upload_id, hash_and_size->sha256_hex,
            [this](const std::filesystem::path& path) { return object_store_->delete_object_path(path); });
    }

    if (!finalize_result.has_value()) {
        return make_error_result<CompleteUploadResult>(finalize_result.error());
    }

    [[maybe_unused]] const bool temp_deleted = object_store_->delete_temp_path(temp_abs_path);
    if (finalize_result->cleanup_old_sha.has_value()) {
        repository_->cleanup_unreferenced_object(*finalize_result->cleanup_old_sha, [this](std::string_view rel_path) {
            return object_store_->delete_object_path(config_.root_dir / rel_path);
        });
    }

    return CompleteUploadResult{
        .upload_id = std::string(upload_id),
        .path = to_public_storage_path(user_id, finalize_result->scoped_path),
        .sha256 = hash_and_size->sha256_hex,
        .size_bytes = hash_and_size->size_bytes,
    };
}

std::expected<CreateDirectoryServiceResult, StorageError> StorageService::create_directory(
    std::int64_t user_id, std::string_view canonical_path) {
    if (user_id <= 0) {
        return make_error_result<CreateDirectoryServiceResult>(StorageError::InternalError);
    }
    if (!validate_canonical_path(canonical_path)) {
        return make_error_result<CreateDirectoryServiceResult>(StorageError::InvalidPath);
    }

    const auto create_result =
        repository_->create_directory_node(user_id, scoped_storage_path(user_id, canonical_path));
    if (!create_result.has_value()) {
        return std::unexpected(create_result.error());
    }
    return CreateDirectoryServiceResult{
        .path = std::string(canonical_path),
    };
}

std::expected<ListDirectoryResult, StorageError> StorageService::list_directory(std::int64_t user_id,
                                                                                std::string_view canonical_path,
                                                                                const DirectoryListOptions& options) {
    if (user_id <= 0) {
        return std::unexpected(StorageError::InternalError);
    }
    if (!validate_canonical_path(canonical_path)) {
        return std::unexpected(StorageError::InvalidPath);
    }

    auto list_result =
        repository_->list_directory_children(user_id, scoped_storage_path(user_id, canonical_path), options);
    if (!list_result.has_value()) {
        return std::unexpected(list_result.error());
    }

    std::vector<TreeItem> items = std::move(*list_result);
    for (TreeItem& item : items) {
        item.path = to_public_storage_path(user_id, item.path);
    }

    return ListDirectoryResult{
        .path = std::string(canonical_path),
        .items = std::move(items),
    };
}

std::expected<DeleteNodeServiceResult, StorageError> StorageService::delete_node(std::int64_t user_id,
                                                                                 std::string_view canonical_path) {
    if (user_id <= 0) {
        return make_error_result<DeleteNodeServiceResult>(StorageError::InternalError);
    }
    if (!validate_canonical_path(canonical_path)) {
        return make_error_result<DeleteNodeServiceResult>(StorageError::InvalidPath);
    }
    if (canonical_path == "/") {
        return make_error_result<DeleteNodeServiceResult>(StorageError::RootDeleteNotAllowed);
    }

    auto delete_result = repository_->delete_node_record(user_id, scoped_storage_path(user_id, canonical_path));
    if (delete_result.has_value() && delete_result->node_type == StorageNodeType::File &&
        delete_result->cleanup_sha.has_value()) {
        repository_->cleanup_unreferenced_object(*delete_result->cleanup_sha, [this](std::string_view rel_path) {
            return object_store_->delete_object_path(config_.root_dir / rel_path);
        });
    }

    if (!delete_result.has_value()) {
        return std::unexpected(delete_result.error());
    }

    return DeleteNodeServiceResult{
        .path = std::string(canonical_path),
        .node_type = delete_result->node_type,
    };
}

std::expected<RecentFilesServiceResult, StorageError> StorageService::list_recent(std::int64_t user_id,
                                                                                  std::int64_t limit) {
    if (user_id <= 0) {
        return make_error_result<RecentFilesServiceResult>(StorageError::InternalError);
    }
    if (limit <= 0 || limit > kMaxRecentLimit) {
        return make_error_result<RecentFilesServiceResult>(StorageError::InvalidLimit);
    }

    auto recent_result = repository_->list_recent_files(user_id, limit);
    if (!recent_result.has_value()) {
        return make_error_result<RecentFilesServiceResult>(recent_result.error());
    }

    std::vector<RecentFileItem> items = std::move(*recent_result);
    for (RecentFileItem& item : items) {
        item.path = to_public_storage_path(user_id, item.path);
    }

    return RecentFilesServiceResult{
        .limit = limit,
        .items = std::move(items),
    };
}

std::expected<StorageUsageSummaryResult, StorageError> StorageService::usage(std::int64_t user_id) {
    if (user_id <= 0) {
        return make_error_result<StorageUsageSummaryResult>(StorageError::InternalError);
    }

    auto usage_result = repository_->collect_storage_usage(user_id);
    if (!usage_result.has_value()) {
        return make_error_result<StorageUsageSummaryResult>(usage_result.error());
    }

    std::unordered_map<std::string, UsageBreakdownItem> breakdown_by_type;
    breakdown_by_type.reserve(usage_result->items.size());
    bool usage_math_saturated = false;
    std::int64_t used_bytes = 0;
    for (const StorageUsageFileItem& item : usage_result->items) {
        const std::string file_type = std::string(classify_file_type(item.path));
        UsageBreakdownItem& breakdown = breakdown_by_type[file_type];
        if (breakdown.file_type.empty()) {
            breakdown.file_type = file_type;
        }
        breakdown.size_bytes = common::saturating_add(breakdown.size_bytes, item.size_bytes, usage_math_saturated);
        breakdown.file_count = common::saturating_add(breakdown.file_count, std::int64_t{1}, usage_math_saturated);
        used_bytes = common::saturating_add(used_bytes, item.size_bytes, usage_math_saturated);
    }

    const std::int64_t total_bytes = usage_result->quota_bytes;
    const std::int64_t available_bytes =
        total_bytes > used_bytes ? common::saturating_sub(total_bytes, used_bytes, usage_math_saturated) : 0;
    const std::int64_t used_percent = rounded_ratio_percent(used_bytes, total_bytes, usage_math_saturated);

    if (usage_math_saturated) {
        common::Logger::instance()
            .warn("storage usage arithmetic saturated")
            .field("user_id", user_id)
            .field("used_bytes", used_bytes)
            .field("quota_bytes", total_bytes)
            .field("decision", "return_clamped_values");
    }

    std::vector<UsageBreakdownItem> breakdown;
    breakdown.reserve(breakdown_by_type.size());
    for (auto& [file_type, item] : breakdown_by_type) {
        if (item.file_type.empty()) {
            item.file_type = file_type;
        }
        breakdown.push_back(std::move(item));
    }
    std::ranges::sort(breakdown, [](const UsageBreakdownItem& left, const UsageBreakdownItem& right) {
        if (left.size_bytes != right.size_bytes) {
            return left.size_bytes > right.size_bytes;
        }
        return left.file_type < right.file_type;
    });

    return StorageUsageSummaryResult{
        .total_bytes = total_bytes,
        .used_bytes = used_bytes,
        .available_bytes = available_bytes,
        .used_percent = used_percent,
        .max_chunk_bytes = static_cast<std::int64_t>(config_.max_body_bytes),
        .max_file_bytes = config_.max_file_bytes,
        .breakdown = std::move(breakdown),
    };
}

std::expected<IssueTicketResult, StorageError> StorageService::issue_download_ticket(std::int64_t user_id,
                                                                                     std::string_view canonical_path) {
    if (user_id <= 0) {
        return make_error_result<IssueTicketResult>(StorageError::InternalError);
    }
    if (!validate_canonical_path(canonical_path) || canonical_path == "/") {
        return make_error_result<IssueTicketResult>(StorageError::InvalidPath);
    }

    const auto target = resolve_download_target(*repository_, config_, user_id, canonical_path);
    if (!target.has_value()) {
        return make_error_result<IssueTicketResult>(target.error());
    }

    const std::string canonical_path_text(canonical_path);
    std::string_view issue_error = "store_download_ticket_retry_exhausted";
    bool stop_retry = false;
    for (int attempt = 0; attempt < kMaxStoreDownloadTicketAttempts; ++attempt) {
        auto issued_ticket =
            nebula::storage::issue_download_ticket(config_.download_ticket_ttl, user_id, canonical_path_text);
        if (!issued_ticket.has_value()) {
            issue_error = to_string(issued_ticket.error());
            break;
        }

        switch (repository_->store_download_ticket(issued_ticket->ticket, user_id, canonical_path_text,
                                                   std::chrono::seconds{issued_ticket->expires_at_s})) {
            case StoreDownloadTicketStatus::Stored:
                return IssueTicketResult{
                    .path = std::string(canonical_path),
                    .ticket = std::move(issued_ticket->ticket),
                    .expires_at_s = issued_ticket->expires_at_s,
                };
            case StoreDownloadTicketStatus::Duplicate:
                continue;
            case StoreDownloadTicketStatus::InternalError:
                issue_error = "store_download_ticket_failed";
                stop_retry = true;
                break;
        }
        if (stop_retry) {
            break;
        }
    }

    common::Logger::instance()
        .error("download ticket issue failed")
        .field("user_id", user_id)
        .field("path", canonical_path)
        .field("error", issue_error)
        .field("decision", "return_internal_error");
    return make_error_result<IssueTicketResult>(StorageError::InternalError);
}

std::expected<PrepareDownloadResult, StorageError> StorageService::prepare_download(std::string_view ticket) {
    auto stored_ticket = repository_->find_download_ticket(ticket);
    if (!stored_ticket.has_value()) {
        switch (stored_ticket.error()) {
            case FindDownloadTicketError::NotFound:
                return make_error_result<PrepareDownloadResult>(StorageError::DownloadTicketInvalid);
            case FindDownloadTicketError::InternalError:
                return make_error_result<PrepareDownloadResult>(StorageError::InternalError);
        }
    }

    const DownloadTicketClaims claims{
        .user_id = stored_ticket->user_id,
        .expires_at_s = stored_ticket->expires_at_s,
        .canonical_path = stored_ticket->canonical_path,
    };
    switch (verify_download_ticket(ticket, claims)) {
        case DownloadTicketVerifyStatus::Valid:
            break;
        case DownloadTicketVerifyStatus::Invalid:
            return make_error_result<PrepareDownloadResult>(StorageError::DownloadTicketInvalid);
        case DownloadTicketVerifyStatus::Expired:
            return make_error_result<PrepareDownloadResult>(StorageError::DownloadTicketExpired);
        case DownloadTicketVerifyStatus::InternalError:
            return make_error_result<PrepareDownloadResult>(StorageError::InternalError);
    }

    const auto target = resolve_download_target(*repository_, config_, claims.user_id, claims.canonical_path);
    if (!target.has_value()) {
        return make_error_result<PrepareDownloadResult>(target.error());
    }

    auto body = object_store_->read_file(config_.root_dir / target->object_rel_path);
    if (!body.has_value()) {
        switch (body.error()) {
            case common::ReadFileError::TooLarge:
                return make_error_result<PrepareDownloadResult>(StorageError::FileTooLarge);
            case common::ReadFileError::OpenFailed:
            case common::ReadFileError::ReadFailed:
                return make_error_result<PrepareDownloadResult>(StorageError::InternalError);
        }
    }
    return PrepareDownloadResult{
        .canonical_path = claims.canonical_path,
        .sha256 = target->sha256,
        .body = std::move(*body),
    };
}

std::expected<GcResult, StorageError> StorageService::run_gc() {
    const StorageGcSnapshot snapshot = repository_->collect_storage_gc_snapshot(config_.upload_session_ttl);
    if (snapshot.status != StorageGcSnapshotStatus::Ready) {
        return make_error_result<GcResult>(StorageError::InternalError);
    }

    std::unordered_set<std::string> object_rel_paths_in_db;
    object_rel_paths_in_db.reserve(snapshot.object_rel_paths_in_db.size());
    for (const std::string& object_rel_path : snapshot.object_rel_paths_in_db) {
        object_rel_paths_in_db.insert(object_rel_path);
    }

    std::unordered_set<std::string> active_temp_rel_paths;
    active_temp_rel_paths.reserve(snapshot.active_temp_rel_paths.size());
    for (const std::string& temp_rel_path : snapshot.active_temp_rel_paths) {
        active_temp_rel_paths.insert(temp_rel_path);
    }

    const std::vector<std::filesystem::path> file_only_objects =
        object_store_->scan_orphan_objects(object_rel_paths_in_db, kFileOnlyOrphanMinAge);
    std::int64_t cleaned_file_only_object_count = 0;
    for (const std::filesystem::path& file_only_object : file_only_objects) {
        const std::string object_rel_path =
            std::filesystem::relative(file_only_object, config_.root_dir).generic_string();
        const CleanupStatus cleanup_status = repository_->cleanup_file_only_object(
            object_rel_path, file_only_object,
            [this](const std::filesystem::path& path) { return object_store_->delete_object_path(path); });
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_file_only_object_count;
        }
    }

    std::int64_t cleaned_object_count = 0;
    for (const GcOrphanObject& orphan : snapshot.orphan_objects) {
        const CleanupStatus cleanup_status =
            repository_->cleanup_unreferenced_object(orphan.sha256, [this](std::string_view rel_path) {
                return object_store_->delete_object_path(config_.root_dir / rel_path);
            });
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_object_count;
        }
    }

    std::int64_t cleaned_temp_count = 0;
    for (const GcExpiredUploadSession& session : snapshot.expired_sessions) {
        const CleanupStatus cleanup_status = repository_->cleanup_temp_file(
            session.temp_rel_path, config_.root_dir / session.temp_rel_path,
            [this](const std::filesystem::path& path) { return object_store_->delete_temp_path(path); });
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_temp_count;
        }
    }

    const std::vector<std::filesystem::path> orphan_temp_files =
        object_store_->scan_orphan_temps(active_temp_rel_paths, kFileOnlyOrphanMinAge);
    std::int64_t cleaned_orphan_temp_count = 0;
    for (const std::filesystem::path& orphan_temp_file : orphan_temp_files) {
        const std::string temp_rel_path =
            std::filesystem::relative(orphan_temp_file, config_.root_dir).generic_string();
        const CleanupStatus cleanup_status = repository_->cleanup_temp_file(
            temp_rel_path, orphan_temp_file,
            [this](const std::filesystem::path& path) { return object_store_->delete_temp_path(path); });
        if (cleanup_status == CleanupStatus::Cleaned) {
            ++cleaned_temp_count;
            ++cleaned_orphan_temp_count;
        }
    }

    return GcResult{
        .expired_upload_sessions = static_cast<std::int64_t>(snapshot.expired_sessions.size()),
        .expired_download_tickets = snapshot.expired_download_ticket_count,
        .cleaned_temp_files = cleaned_temp_count,
        .unreferenced_objects = static_cast<std::int64_t>(snapshot.orphan_objects.size()),
        .cleaned_unreferenced_objects = cleaned_object_count,
        .file_only_objects = static_cast<std::int64_t>(file_only_objects.size()),
        .cleaned_file_only_objects = cleaned_file_only_object_count,
        .orphan_temp_files = static_cast<std::int64_t>(orphan_temp_files.size()),
        .cleaned_orphan_temp_files = cleaned_orphan_temp_count,
    };
}

}  // namespace nebula::storage
