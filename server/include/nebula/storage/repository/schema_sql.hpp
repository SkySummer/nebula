#ifndef NEBULA_STORAGE_REPOSITORY_SCHEMA_SQL_HPP
#define NEBULA_STORAGE_REPOSITORY_SCHEMA_SQL_HPP

#include <string_view>

namespace nebula::storage {

inline constexpr std::string_view kCheckStorageRepositorySchemaReadySql = R"sql(
SELECT to_regclass('public.users'),
       to_regclass('public.storage_objects'),
       to_regclass('public.storage_nodes'),
       to_regclass('public.storage_upload_sessions'),
       to_regclass('public.storage_download_tickets'),
       NULLIF(
           (
               SELECT COUNT(*) = 1
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'users'
                 AND column_name IN ('quota_bytes')
           ),
           FALSE
       ),
       NULLIF(
           (
               SELECT COUNT(*) = 6
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'storage_objects'
                 AND column_name IN (
                     'sha256',
                     'size_bytes',
                     'object_rel_path',
                     'ref_count',
                     'created_at_s',
                     'updated_at_s'
                 )
           ),
           FALSE
       ),
       NULLIF(
           (
               SELECT COUNT(*) = 5
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'storage_nodes'
                 AND column_name IN (
                     'path',
                     'node_type',
                     'sha256',
                     'size_bytes',
                     'updated_at_s'
                 )
           ),
           FALSE
       ),
       NULLIF(
           (
               SELECT COUNT(*) = 8
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'storage_upload_sessions'
                 AND column_name IN (
                     'upload_id',
                     'path',
                     'temp_rel_path',
                     'total_chunks',
                     'next_chunk_index',
                     'temp_size_bytes',
                     'created_at_s',
                     'updated_at_s'
                 )
           ),
           FALSE
       ),
       NULLIF(
           (
               SELECT COUNT(*) = 5
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'storage_download_tickets'
                 AND column_name IN (
                     'ticket',
                     'user_id',
                     'canonical_path',
                     'created_at_s',
                     'expires_at_s'
                 )
           ),
           FALSE
       )
)sql";

inline constexpr std::string_view kAcquireExclusiveAdvisoryXactLockSql = R"sql(
SELECT pg_advisory_xact_lock($1::bigint)
)sql";

inline constexpr std::string_view kAcquireSharedAdvisoryXactLockSql = R"sql(
SELECT pg_advisory_xact_lock_shared($1::bigint)
)sql";

inline constexpr std::string_view kAcquireHashedTextAdvisoryXactLockSql = R"sql(
SELECT pg_advisory_xact_lock(hashtextextended($1::text, 0))
)sql";

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_SCHEMA_SQL_HPP
