#ifndef NEBULA_STORAGE_REPOSITORY_FILE_NODES_SQL_HPP
#define NEBULA_STORAGE_REPOSITORY_FILE_NODES_SQL_HPP

#include <string_view>

namespace nebula::storage {

inline constexpr std::string_view kEnsureUserRootDirectorySql = R"sql(
INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s)
VALUES($1, 'directory', NULL, NULL, $2::bigint)
ON CONFLICT (path) DO NOTHING
)sql";

inline constexpr std::string_view kFindStorageNodeTypeForUpdateSql = R"sql(
SELECT node_type
FROM storage_nodes
WHERE path = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kFindStorageNodeDescendantSql = R"sql(
SELECT 1
FROM storage_nodes
WHERE path LIKE $1 ESCAPE '\'
LIMIT 1
)sql";

inline constexpr std::string_view kFindUserQuotaBytesSql = R"sql(
SELECT quota_bytes
FROM users
WHERE user_id = $1::bigint
LIMIT 1
)sql";

inline constexpr std::string_view kSumUserFileBytesSql = R"sql(
SELECT COALESCE(SUM(size_bytes), 0)
FROM storage_nodes
WHERE path LIKE $1 ESCAPE '\'
  AND node_type = 'file'
)sql";

inline constexpr std::string_view kFindExistingFileTargetForUpdateSql = R"sql(
SELECT node_type,
       sha256,
       size_bytes
FROM storage_nodes
WHERE path = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kFindFileNodeSql = R"sql(
SELECT n.path,
       n.node_type,
       n.sha256,
       n.size_bytes,
       o.object_rel_path
FROM storage_nodes n
LEFT JOIN storage_objects o ON n.sha256 = o.sha256
WHERE n.path = $1
LIMIT 1
)sql";

inline constexpr std::string_view kInsertDirectoryNodeSql = R"sql(
INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s)
VALUES($1, 'directory', NULL, NULL, $2::bigint)
)sql";

inline constexpr std::string_view kListDirectoryChildrenSqlPrefix = R"sql(
SELECT path,
       node_type,
       size_bytes,
       updated_at_s
FROM storage_nodes
WHERE path LIKE $1 ESCAPE '\'
ORDER BY
)sql";

inline constexpr std::string_view kListRecentFilesSql = R"sql(
SELECT path,
       size_bytes,
       updated_at_s
FROM storage_nodes
WHERE path LIKE $1 ESCAPE '\'
  AND node_type = 'file'
ORDER BY updated_at_s DESC, path ASC
LIMIT $2::bigint
)sql";

inline constexpr std::string_view kListStorageUsageFilesSql = R"sql(
SELECT path,
       size_bytes
FROM storage_nodes
WHERE path LIKE $1 ESCAPE '\'
  AND node_type = 'file'
)sql";

inline constexpr std::string_view kUpsertStorageObjectSql = R"sql(
INSERT INTO storage_objects(
    sha256,
    size_bytes,
    object_rel_path,
    ref_count,
    created_at_s,
    updated_at_s
)
VALUES($1, $2::bigint, $3, 1, $4::bigint, $4::bigint)
ON CONFLICT (sha256) DO UPDATE
SET ref_count = storage_objects.ref_count + 1,
    updated_at_s = $4::bigint
)sql";

inline constexpr std::string_view kInsertFileNodeSql = R"sql(
INSERT INTO storage_nodes(path, node_type, sha256, size_bytes, updated_at_s)
VALUES($1, 'file', $2, $3::bigint, $4::bigint)
)sql";

inline constexpr std::string_view kUpdateFileNodeWithObjectSql = R"sql(
UPDATE storage_nodes
SET node_type = 'file',
    sha256 = $2,
    size_bytes = $3::bigint,
    updated_at_s = $4::bigint
WHERE path = $1
)sql";

inline constexpr std::string_view kUpdateFileNodeSizeSql = R"sql(
UPDATE storage_nodes
SET node_type = 'file',
    size_bytes = $2::bigint,
    updated_at_s = $3::bigint
WHERE path = $1
)sql";

inline constexpr std::string_view kDecrementStorageObjectRefCountSql = R"sql(
UPDATE storage_objects
SET ref_count = ref_count - 1,
    updated_at_s = $2::bigint
WHERE sha256 = $1
RETURNING ref_count
)sql";

inline constexpr std::string_view kFindNodeForDeleteSql = R"sql(
SELECT node_type,
       sha256
FROM storage_nodes
WHERE path = $1
FOR UPDATE
)sql";

inline constexpr std::string_view kDeleteNodeSql = R"sql(
DELETE FROM storage_nodes
WHERE path = $1
)sql";

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_FILE_NODES_SQL_HPP
