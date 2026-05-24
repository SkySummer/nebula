#ifndef NEBULA_AUTH_REPOSITORY_SQL_HPP
#define NEBULA_AUTH_REPOSITORY_SQL_HPP

#include <string_view>

namespace nebula::auth {

inline constexpr std::string_view kCheckAuthRepositorySchemaReadySql = R"sql(
SELECT to_regclass('public.users'),
       to_regclass('public.users_user_id_seq'),
       NULLIF(
           (
               SELECT COUNT(*) = 11
               FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name = 'users'
                 AND column_name IN (
                     'user_id',
                     'username',
                     'password_hash_algorithm',
                     'password_hash_iterations',
                     'password_hash_salt',
                     'password_hash_derived_key',
                     'role',
                     'status',
                     'created_at_s',
                     'token_version',
                     'quota_bytes'
                 )
           ),
           FALSE
       )
)sql";

inline constexpr std::string_view kAcquireExclusiveAdvisoryXactLockSql = R"sql(
SELECT pg_advisory_xact_lock($1::bigint)
)sql";

inline constexpr std::string_view kCreateUserSql = R"sql(
WITH inserted AS (
    INSERT INTO users(
        username,
        password_hash_algorithm,
        password_hash_iterations,
        password_hash_salt,
        password_hash_derived_key,
        role,
        status,
        created_at_s,
        token_version
    )
    SELECT
        $1,
        $2,
        $3::bigint,
        $4,
        $5,
        CASE WHEN EXISTS(SELECT 1 FROM users) THEN 'user' ELSE 'owner' END,
        'active',
        $6::bigint,
        1
    ON CONFLICT (username) DO NOTHING
    RETURNING
        user_id,
        username,
        password_hash_algorithm,
        password_hash_iterations,
        password_hash_salt,
        password_hash_derived_key,
        role,
        status,
        created_at_s,
        token_version
)
SELECT *
FROM inserted
)sql";

inline constexpr std::string_view kFindUserByUsernameSql = R"sql(
SELECT user_id,
       username,
       password_hash_algorithm,
       password_hash_iterations,
       password_hash_salt,
       password_hash_derived_key,
       role,
       status,
       created_at_s,
       token_version
FROM users
WHERE username = $1
LIMIT 1
)sql";

inline constexpr std::string_view kFindUserByIdSql = R"sql(
SELECT user_id,
       username,
       password_hash_algorithm,
       password_hash_iterations,
       password_hash_salt,
       password_hash_derived_key,
       role,
       status,
       created_at_s,
       token_version
FROM users
WHERE user_id = $1::bigint
LIMIT 1
)sql";

inline constexpr std::string_view kUpdateUserPasswordHashSql = R"sql(
UPDATE users
SET password_hash_algorithm = $2,
    password_hash_iterations = $3::bigint,
    password_hash_salt = $4,
    password_hash_derived_key = $5,
    token_version = $6::bigint
WHERE user_id = $1::bigint
RETURNING user_id,
          username,
          password_hash_algorithm,
          password_hash_iterations,
          password_hash_salt,
          password_hash_derived_key,
          role,
          status,
          created_at_s,
          token_version
)sql";

inline constexpr std::string_view kListUsersSql = R"sql(
SELECT user_id,
       username,
       role,
       status,
       created_at_s
FROM users
ORDER BY user_id ASC
LIMIT $1::bigint
OFFSET $2::bigint
)sql";

inline constexpr std::string_view kFindUserProfileForUpdateSql = R"sql(
SELECT user_id,
       username,
       role,
       status,
       created_at_s
FROM users
WHERE user_id = $1::bigint
LIMIT 1
FOR UPDATE
)sql";

inline constexpr std::string_view kCountActiveOwnersSql = R"sql(
SELECT COUNT(*)
FROM users
WHERE role = 'owner' AND status = 'active'
)sql";

inline constexpr std::string_view kUpdateUserRoleAndStatusSql = R"sql(
UPDATE users
SET role = $2,
    status = $3
WHERE user_id = $1::bigint
RETURNING user_id,
          username,
          role,
          status,
          created_at_s
)sql";

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_REPOSITORY_SQL_HPP
