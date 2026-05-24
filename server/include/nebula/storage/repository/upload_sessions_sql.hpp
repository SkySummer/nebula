#ifndef NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_SQL_HPP
#define NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_SQL_HPP

#include <string_view>

namespace nebula::storage {

inline constexpr std::string_view kCreateUploadSessionSql = R"sql(
INSERT INTO storage_upload_sessions(
    upload_id,
    path,
    temp_rel_path,
    total_chunks,
    next_chunk_index,
    temp_size_bytes,
    created_at_s,
    updated_at_s
)
VALUES($1, $2, $3, $4::bigint, 0, $5::bigint, $6::bigint, $6::bigint)
ON CONFLICT (upload_id) DO NOTHING
RETURNING upload_id
)sql";

inline constexpr std::string_view kFindUploadSessionForAppendSql = R"sql(
SELECT path,
       temp_rel_path,
       total_chunks,
       next_chunk_index,
       temp_size_bytes
FROM storage_upload_sessions
WHERE upload_id = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kAdvanceUploadSessionSql = R"sql(
UPDATE storage_upload_sessions
SET next_chunk_index = next_chunk_index + 1,
    temp_size_bytes = $3::bigint,
    updated_at_s = $2::bigint
WHERE upload_id = $1
RETURNING next_chunk_index
)sql";

inline constexpr std::string_view kFindUploadSessionForPrepareSql = R"sql(
SELECT path,
       temp_rel_path,
       total_chunks,
       next_chunk_index,
       temp_size_bytes
FROM storage_upload_sessions
WHERE upload_id = $1
LIMIT 1
)sql";

inline constexpr std::string_view kFindUploadSessionForFinalizeSql = R"sql(
SELECT path,
       total_chunks,
       next_chunk_index,
       temp_size_bytes
FROM storage_upload_sessions
WHERE upload_id = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kDeleteUploadSessionSql = R"sql(
DELETE FROM storage_upload_sessions
WHERE upload_id = $1
)sql";

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_SQL_HPP
