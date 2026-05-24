#ifndef NEBULA_STORAGE_REPOSITORY_GC_SQL_HPP
#define NEBULA_STORAGE_REPOSITORY_GC_SQL_HPP

#include <string_view>

namespace nebula::storage {

inline constexpr std::string_view kFindExpiredUploadSessionsSql = R"sql(
SELECT upload_id,
       temp_rel_path
FROM storage_upload_sessions
WHERE updated_at_s < $1::bigint
)sql";

inline constexpr std::string_view kDeleteExpiredUploadSessionsSql = R"sql(
DELETE FROM storage_upload_sessions
WHERE updated_at_s < $1::bigint
)sql";

inline constexpr std::string_view kDeleteExpiredDownloadTicketsSql = R"sql(
DELETE FROM storage_download_tickets
WHERE expires_at_s <= $1::bigint
RETURNING ticket
)sql";

inline constexpr std::string_view kListActiveTempRelPathsSql = R"sql(
SELECT temp_rel_path
FROM storage_upload_sessions
)sql";

inline constexpr std::string_view kSyncStorageObjectRefCountsSql = R"sql(
UPDATE storage_objects o
SET ref_count = c.ref_cnt,
    updated_at_s = $1::bigint
FROM (
    SELECT sha256, COUNT(*)::bigint AS ref_cnt
    FROM storage_nodes
    WHERE node_type = 'file'
    GROUP BY sha256
) c
WHERE o.sha256 = c.sha256
  AND o.ref_count <> c.ref_cnt
)sql";

inline constexpr std::string_view kResetOrphanStorageObjectRefCountsSql = R"sql(
UPDATE storage_objects o
SET ref_count = 0,
    updated_at_s = $1::bigint
WHERE NOT EXISTS (
          SELECT 1
          FROM storage_nodes n
          WHERE n.node_type = 'file'
            AND n.sha256 = o.sha256
      )
  AND o.ref_count <> 0
)sql";

inline constexpr std::string_view kListObjectRelPathsSql = R"sql(
SELECT object_rel_path
FROM storage_objects
)sql";

inline constexpr std::string_view kListOrphanStorageObjectsSql = R"sql(
SELECT sha256,
       object_rel_path
FROM storage_objects
WHERE ref_count <= 0
)sql";

inline constexpr std::string_view kFindCleanupCandidateObjectSql = R"sql(
SELECT object_rel_path,
       ref_count,
       EXISTS (
           SELECT 1
           FROM storage_nodes n
           WHERE n.node_type = 'file'
             AND n.sha256 = $1
       )
FROM storage_objects
WHERE sha256 = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kDeleteCleanupCandidateObjectSql = R"sql(
DELETE FROM storage_objects
WHERE sha256 = $1
  AND ref_count <= 0
)sql";

inline constexpr std::string_view kFindStorageObjectByRelPathSql = R"sql(
SELECT 1
FROM storage_objects
WHERE object_rel_path = $1
LIMIT 1
)sql";

inline constexpr std::string_view kFindUploadSessionByTempRelPathSql = R"sql(
SELECT 1
FROM storage_upload_sessions
WHERE temp_rel_path = $1
LIMIT 1
)sql";

inline constexpr std::string_view kFindUploadFailureObjectReferenceSql = R"sql(
SELECT 1
WHERE EXISTS (
          SELECT 1
          FROM storage_objects
          WHERE sha256 = $1
            AND ref_count > 0
      )
   OR EXISTS (
          SELECT 1
          FROM storage_nodes
          WHERE node_type = 'file'
            AND sha256 = $1
      )
)sql";

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_GC_SQL_HPP
