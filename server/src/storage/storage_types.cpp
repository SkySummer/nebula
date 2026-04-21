#include "nebula/storage/storage_types.hpp"

#include <optional>
#include <string_view>

namespace nebula::storage {

std::string_view to_string(StorageNodeType type) noexcept {
    switch (type) {
        case StorageNodeType::File:
            return "file";
        case StorageNodeType::Directory:
            return "directory";
    }
    return "unknown";
}

std::optional<StorageNodeType> parse_storage_node_type(std::string_view type) noexcept {
    if (type == to_string(StorageNodeType::File)) {
        return StorageNodeType::File;
    }
    if (type == to_string(StorageNodeType::Directory)) {
        return StorageNodeType::Directory;
    }
    return std::nullopt;
}

}  // namespace nebula::storage
