#ifndef NEBULA_STORAGE_REPOSITORY_FILE_NODES_ROW_PARSER_HPP
#define NEBULA_STORAGE_REPOSITORY_FILE_NODES_ROW_PARSER_HPP

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

#include "nebula/storage/domain/types.hpp"

namespace nebula::storage {

struct ExistingFileTargetRow {
    StorageNodeType node_type = StorageNodeType::File;
    std::optional<std::string> sha256;
    std::optional<std::int64_t> size_bytes;
};

struct FileNodeLookupRow {
    std::string path;
    StorageNodeType node_type = StorageNodeType::File;
    std::optional<std::string> sha256;
    std::optional<std::int64_t> size_bytes;
    std::optional<std::string> object_rel_path;
};

struct DirectoryChildRow {
    std::string path;
    StorageNodeType node_type = StorageNodeType::File;
    std::optional<std::int64_t> size_bytes;
    std::int64_t updated_at_s = 0;
};

struct NodeDeleteRow {
    StorageNodeType node_type = StorageNodeType::File;
    std::optional<std::string> sha256;
};

std::optional<StorageNodeType> parse_node_type_row(const pqxx::row& row);

std::optional<std::int64_t> parse_quota_bytes_row(const pqxx::row& row);

std::optional<std::int64_t> parse_total_size_bytes_row(const pqxx::row& row);

std::optional<ExistingFileTargetRow> parse_existing_file_target_row(const pqxx::row& row);

std::optional<FileNodeLookupRow> parse_file_node_lookup_row(const pqxx::row& row);

std::optional<DirectoryChildRow> parse_directory_child_row(const pqxx::row& row);

std::optional<RecentFileItem> parse_recent_file_row(const pqxx::row& row);

std::optional<StorageUsageFileItem> parse_storage_usage_file_row(const pqxx::row& row);

std::optional<std::int64_t> parse_storage_object_ref_count_row(const pqxx::row& row);

std::optional<NodeDeleteRow> parse_node_delete_row(const pqxx::row& row);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_FILE_NODES_ROW_PARSER_HPP
