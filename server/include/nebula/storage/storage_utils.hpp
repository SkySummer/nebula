#ifndef NEBULA_STORAGE_STORAGE_UTILS_HPP
#define NEBULA_STORAGE_STORAGE_UTILS_HPP

#include <filesystem>

namespace nebula::storage {

bool delete_file_if_exists(const std::filesystem::path& path);

void try_cleanup_empty_parents(const std::filesystem::path& file_path, const std::filesystem::path& stop_dir);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_STORAGE_UTILS_HPP
