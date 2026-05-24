#ifndef NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_ROW_PARSER_HPP
#define NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_ROW_PARSER_HPP

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

namespace nebula::storage {

struct UploadSessionChunkRow {
    std::string path;
    std::string temp_rel_path;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
    std::optional<std::int64_t> temp_size_bytes;
};

struct UploadSessionFinalizeRow {
    std::string path;
    std::int64_t total_chunks = 0;
    std::int64_t next_chunk_index = 0;
    std::optional<std::int64_t> temp_size_bytes;
};

std::optional<UploadSessionChunkRow> parse_upload_session_chunk_row(const pqxx::row& row);

std::optional<UploadSessionFinalizeRow> parse_upload_session_finalize_row(const pqxx::row& row);

std::optional<std::int64_t> parse_next_chunk_index_row(const pqxx::row& row);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_ROW_PARSER_HPP
