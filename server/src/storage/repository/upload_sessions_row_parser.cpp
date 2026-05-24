#include "nebula/storage/repository/upload_sessions_row_parser.hpp"

#include "nebula/database/row_check.hpp"

namespace nebula::storage {

std::optional<UploadSessionChunkRow> parse_upload_session_chunk_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 5, {0, 1, 2, 3}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    UploadSessionChunkRow parsed;
    parsed.path = row[0].as<std::string>();
    parsed.temp_rel_path = row[1].as<std::string>();
    parsed.total_chunks = row[2].as<std::int64_t>(0);
    parsed.next_chunk_index = row[3].as<std::int64_t>(0);
    if (!row[4].is_null()) {
        parsed.temp_size_bytes = row[4].as<std::int64_t>(0);
    }
    return parsed;
}

std::optional<UploadSessionFinalizeRow> parse_upload_session_finalize_row(const pqxx::row& row) {
    if (database::check_row_required_fields(row, 4, {0, 1, 2}) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    UploadSessionFinalizeRow parsed;
    parsed.path = row[0].as<std::string>();
    parsed.total_chunks = row[1].as<std::int64_t>(0);
    parsed.next_chunk_index = row[2].as<std::int64_t>(0);
    if (!row[3].is_null()) {
        parsed.temp_size_bytes = row[3].as<std::int64_t>(0);
    }
    return parsed;
}

std::optional<std::int64_t> parse_next_chunk_index_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 1) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }
    return row[0].as<std::int64_t>(0);
}

}  // namespace nebula::storage
