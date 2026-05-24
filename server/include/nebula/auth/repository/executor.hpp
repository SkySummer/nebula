#ifndef NEBULA_AUTH_REPOSITORY_EXECUTOR_HPP
#define NEBULA_AUTH_REPOSITORY_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

#include "nebula/auth/domain/password_hash.hpp"
#include "nebula/auth/domain/user.hpp"

namespace nebula::auth {

pqxx::row execute_check_auth_repository_schema_ready(pqxx::read_transaction& tx);

void execute_acquire_global_users_mutation_advisory_lock(pqxx::work& tx);

pqxx::result execute_create_user(pqxx::work& tx, std::string_view username, const PasswordHashValue& password_hash,
                                 std::int64_t now_s);

pqxx::result execute_find_user_by_username(pqxx::read_transaction& tx, std::string_view username);

pqxx::result execute_find_user_by_id(pqxx::read_transaction& tx, std::int64_t user_id);

pqxx::result execute_update_user_password_hash(pqxx::work& tx, std::int64_t user_id,
                                               const PasswordHashValue& password_hash, std::int64_t token_version);

pqxx::result execute_list_users(pqxx::read_transaction& tx, std::int64_t limit, std::int64_t offset);

pqxx::result execute_find_user_profile_for_update(pqxx::work& tx, std::int64_t user_id);

std::int64_t execute_count_active_owners(pqxx::work& tx);

pqxx::result execute_update_user(pqxx::work& tx, std::int64_t user_id, UserRole role, UserStatus status);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_REPOSITORY_EXECUTOR_HPP
