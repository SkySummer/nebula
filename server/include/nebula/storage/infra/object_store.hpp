#ifndef NEBULA_STORAGE_INFRA_OBJECT_STORE_HPP
#define NEBULA_STORAGE_INFRA_OBJECT_STORE_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "nebula/common/platform/file_io.hpp"
#include "nebula/storage/domain/types.hpp"

namespace nebula::storage {

enum class AppendTempChunkStatus : std::uint8_t {
    Appended,
    FileTooLarge,
    Failed,
};

struct AppendTempChunkResult {
    AppendTempChunkStatus status = AppendTempChunkStatus::Failed;
    std::int64_t bytes_written = 0;
};

enum class PublishObjectStatus : std::uint8_t {
    Created,
    Failed,
    AlreadyExists,
};

class ObjectStore {
public:
    explicit ObjectStore(StorageRuntimeConfig config);

    [[nodiscard]] bool ensure_root_dirs() const;
    [[nodiscard]] static bool create_empty_temp_file(const std::filesystem::path& temp_path);
    [[nodiscard]] AppendTempChunkResult append_temp_chunk(const std::filesystem::path& temp_path, std::string_view body,
                                                          std::int64_t committed_size_bytes) const;
    [[nodiscard]] static bool restore_temp_size(const std::filesystem::path& temp_path,
                                                std::int64_t committed_size_bytes);
    [[nodiscard]] static std::optional<HashAndSize> hash_file(const std::filesystem::path& path);
    [[nodiscard]] static PublishObjectStatus publish_object(const std::filesystem::path& temp_path,
                                                            const std::filesystem::path& object_path,
                                                            std::string_view expected_sha256,
                                                            std::int64_t expected_size_bytes);
    [[nodiscard]] std::expected<std::string, common::ReadFileError> read_file(const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<std::filesystem::path> scan_orphan_objects(
        const std::unordered_set<std::string>& object_rel_paths_in_db, std::chrono::seconds min_age_s) const;
    [[nodiscard]] std::vector<std::filesystem::path> scan_orphan_temps(
        const std::unordered_set<std::string>& active_temp_rel_paths, std::chrono::seconds min_age_s) const;
    [[nodiscard]] bool delete_object_path(const std::filesystem::path& object_path) const;
    [[nodiscard]] bool delete_temp_path(const std::filesystem::path& temp_path) const;

private:
    [[nodiscard]] static bool delete_path(const std::filesystem::path& path, const std::filesystem::path& cleanup_root);

    StorageRuntimeConfig config_;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_INFRA_OBJECT_STORE_HPP
