#ifndef NEBULA_STORAGE_DOMAIN_PATH_HPP
#define NEBULA_STORAGE_DOMAIN_PATH_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace nebula::storage {

enum class PathDecodeError : std::uint8_t {
    InvalidEncoding,
    InvalidCanonicalPath,
};

[[nodiscard]] std::string_view to_string(PathDecodeError error) noexcept;

[[nodiscard]] bool validate_canonical_path(std::string_view path);

[[nodiscard]] std::expected<std::string, PathDecodeError> decode_and_validate_path(std::string_view path_b64);

bool delete_file_if_exists(const std::filesystem::path& path);

void try_cleanup_empty_parents(const std::filesystem::path& file_path, const std::filesystem::path& stop_dir);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_DOMAIN_PATH_HPP
