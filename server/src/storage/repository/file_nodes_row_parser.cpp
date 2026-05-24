#include "nebula/storage/repository/file_nodes_row_parser.hpp"

#include "nebula/database/row_check.hpp"

namespace nebula::storage {

std::optional<StorageNodeType> parse_node_type_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return parse_storage_node_type(row[0].as<std::string>());
}

std::optional<std::int64_t> parse_quota_bytes_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return row[0].as<std::int64_t>(0);
}

std::optional<std::int64_t> parse_total_size_bytes_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return row[0].as<std::int64_t>(0);
}

std::optional<ExistingFileTargetRow> parse_existing_file_target_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 3, {0}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const std::optional<StorageNodeType> node_type = parse_storage_node_type(row[0].as<std::string>());
    if (!node_type.has_value()) {
        return std::nullopt;
    }

    ExistingFileTargetRow parsed;
    parsed.node_type = *node_type;
    if (!row[1].is_null()) {
        parsed.sha256 = row[1].as<std::string>();
    }
    if (!row[2].is_null()) {
        parsed.size_bytes = row[2].as<std::int64_t>(0);
    }
    return parsed;
}

std::optional<FileNodeLookupRow> parse_file_node_lookup_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 5, {0, 1}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const std::optional<StorageNodeType> node_type = parse_storage_node_type(row[1].as<std::string>());
    if (!node_type.has_value()) {
        return std::nullopt;
    }

    FileNodeLookupRow parsed;
    parsed.path = row[0].as<std::string>();
    parsed.node_type = *node_type;
    if (!row[2].is_null()) {
        parsed.sha256 = row[2].as<std::string>();
    }
    if (!row[3].is_null()) {
        parsed.size_bytes = row[3].as<std::int64_t>(0);
    }
    if (!row[4].is_null()) {
        parsed.object_rel_path = row[4].as<std::string>();
    }
    return parsed;
}

std::optional<DirectoryChildRow> parse_directory_child_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 4, {0, 1, 3}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const std::optional<StorageNodeType> node_type = parse_storage_node_type(row[1].as<std::string>());
    if (!node_type.has_value()) {
        return std::nullopt;
    }

    DirectoryChildRow parsed;
    parsed.path = row[0].as<std::string>();
    parsed.node_type = *node_type;
    parsed.updated_at_s = row[3].as<std::int64_t>(0);
    if (!row[2].is_null()) {
        parsed.size_bytes = row[2].as<std::int64_t>(0);
    }
    return parsed;
}

std::optional<RecentFileItem> parse_recent_file_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 3) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return RecentFileItem{
        .path = row[0].as<std::string>(),
        .size_bytes = row[1].as<std::int64_t>(0),
        .updated_at_s = row[2].as<std::int64_t>(0),
    };
}

std::optional<StorageUsageFileItem> parse_storage_usage_file_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 2) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return StorageUsageFileItem{
        .path = row[0].as<std::string>(),
        .size_bytes = row[1].as<std::int64_t>(0),
    };
}

std::optional<std::int64_t> parse_storage_object_ref_count_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return row[0].as<std::int64_t>(0);
}

std::optional<NodeDeleteRow> parse_node_delete_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 2, {0}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const std::optional<StorageNodeType> node_type = parse_storage_node_type(row[0].as<std::string>());
    if (!node_type.has_value()) {
        return std::nullopt;
    }

    NodeDeleteRow parsed;
    parsed.node_type = *node_type;
    if (!row[1].is_null()) {
        parsed.sha256 = row[1].as<std::string>();
    }
    return parsed;
}

}  // namespace nebula::storage
