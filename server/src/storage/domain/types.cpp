#include "nebula/storage/domain/types.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace nebula::storage {

std::string_view to_string(StorageNodeType type) noexcept {
    switch (type) {
        case StorageNodeType::File:
            return "file";
        case StorageNodeType::Directory:
            return "directory";
    }
    std::unreachable();
}

std::optional<StorageNodeType> parse_storage_node_type(std::string_view type) noexcept {
    if (type == "file") {
        return StorageNodeType::File;
    }
    if (type == "directory") {
        return StorageNodeType::Directory;
    }
    return std::nullopt;
}

}  // namespace nebula::storage
