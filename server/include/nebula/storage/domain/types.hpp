#ifndef NEBULA_STORAGE_DOMAIN_TYPES_HPP
#define NEBULA_STORAGE_DOMAIN_TYPES_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nebula::storage {

enum class StorageNodeType : std::uint8_t {
    File,
    Directory,
};

[[nodiscard]] std::string_view to_string(StorageNodeType type) noexcept;

[[nodiscard]] std::optional<StorageNodeType> parse_storage_node_type(std::string_view type) noexcept;

enum class DirectoryListSortBy : std::uint8_t {
    Name,
    UpdatedAt,
};

enum class DirectoryListSortDirection : std::uint8_t {
    Asc,
    Desc,
};

struct DirectoryListOptions {
    DirectoryListSortBy sort_by = DirectoryListSortBy::Name;
    DirectoryListSortDirection sort_direction = DirectoryListSortDirection::Asc;
};

struct StorageRuntimeConfig {
    std::filesystem::path root_dir;
    std::filesystem::path temp_dir;
    std::filesystem::path objects_dir;
    std::chrono::seconds upload_session_ttl{86400};
    std::chrono::seconds download_ticket_ttl{120};
    std::size_t max_body_bytes = std::size_t{8} * 1024U * 1024U;
    std::int64_t max_file_bytes = 512LL * 1024 * 1024;
};

struct UploadSessionRecord {
    std::string upload_id;
    std::string path;
    std::string temp_rel_path;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
    std::int64_t temp_size_bytes = 0;
};

struct FileNodeRecord {
    std::string path;
    std::string sha256;
    std::int64_t size_bytes = 0;
    std::string object_rel_path;
};

struct TreeItem {
    std::string name;
    std::string path;
    StorageNodeType node_type = StorageNodeType::File;
    std::int64_t size_bytes = 0;
    std::int64_t updated_at_s = 0;
    std::int64_t file_count = 0;
};

struct RecentFileItem {
    std::string path;
    std::int64_t size_bytes = 0;
    std::int64_t updated_at_s = 0;
};

struct StorageUsageFileItem {
    std::string path;
    std::int64_t size_bytes = 0;
};

struct UploadInitRequest {
    std::string path_b64;
    std::int64_t total_chunks = 0;
};

struct HashAndSize {
    std::string sha256_hex;
    std::int64_t size_bytes = 0;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_DOMAIN_TYPES_HPP
