#ifndef NEBULA_STORAGE_BOOTSTRAP_CONFIG_HPP
#define NEBULA_STORAGE_BOOTSTRAP_CONFIG_HPP

#include <cstdint>
#include <filesystem>

namespace nebula::storage {

inline constexpr std::int64_t kMaxStorageUploadSessionTtlSeconds = 2'592'000;
inline constexpr std::int64_t kMaxStorageDownloadTicketTtlSeconds = 2'592'000;

struct StorageConfig {
    std::filesystem::path root_dir = "runtime/files";
    std::int64_t upload_session_ttl_s = 86400;
    std::int64_t download_ticket_ttl_s = 120;
    std::int64_t max_file_bytes = 512LL * 1024 * 1024;

    [[nodiscard]] bool validate() const;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_BOOTSTRAP_CONFIG_HPP
