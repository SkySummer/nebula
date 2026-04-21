#include "nebula/storage/storage_utils.hpp"

namespace nebula::storage {

bool delete_file_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

void try_cleanup_empty_parents(const std::filesystem::path& file_path, const std::filesystem::path& stop_dir) {
    std::error_code ec;
    std::filesystem::path current = file_path.parent_path();
    while (!current.empty() && current != stop_dir && current.string().size() >= stop_dir.string().size()) {
        ec.clear();
        const bool removed = std::filesystem::remove(current, ec);
        if (ec || !removed) {
            return;
        }
        current = current.parent_path();
    }
}

}  // namespace nebula::storage
