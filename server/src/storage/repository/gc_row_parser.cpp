#include "nebula/storage/repository/gc_row_parser.hpp"

#include "nebula/database/row_check.hpp"

namespace nebula::storage {

std::optional<GcExpiredUploadSession> parse_gc_expired_upload_session_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 2) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return GcExpiredUploadSession{
        .upload_id = row[0].as<std::string>(),
        .temp_rel_path = row[1].as<std::string>(),
    };
}

std::optional<std::string> parse_string_value_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return row[0].as<std::string>();
}

std::optional<GcOrphanObject> parse_gc_orphan_object_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 2) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return GcOrphanObject{
        .sha256 = row[0].as<std::string>(),
        .object_rel_path = row[1].as<std::string>(),
    };
}

std::optional<UnreferencedObjectCleanupRow> parse_unreferenced_object_cleanup_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 3) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return UnreferencedObjectCleanupRow{
        .object_rel_path = row[0].as<std::string>(),
        .ref_count = row[1].as<std::int64_t>(0),
        .has_file_node_reference = row[2].as<bool>(false),
    };
}

}  // namespace nebula::storage
