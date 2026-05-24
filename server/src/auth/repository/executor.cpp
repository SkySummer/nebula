#include "nebula/auth/repository/executor.hpp"

#include <string>

#include "nebula/auth/repository/sql.hpp"
#include "nebula/auth/repository/types.hpp"

namespace nebula::auth {

pqxx::row execute_check_auth_repository_schema_ready(pqxx::read_transaction& tx) {
    return tx.exec1(std::string(kCheckAuthRepositorySchemaReadySql));
}

void execute_acquire_global_users_mutation_advisory_lock(pqxx::work& tx) {
    tx.exec_params(std::string(kAcquireExclusiveAdvisoryXactLockSql), kGlobalUsersMutationAdvisoryLockKey);
}

pqxx::result execute_create_user(pqxx::work& tx, std::string_view username, const PasswordHashValue& password_hash,
                                 std::int64_t now_s) {
    return tx.exec_params(std::string(kCreateUserSql), std::string(username), password_hash.algorithm,
                          password_hash.iterations, password_hash.salt, password_hash.derived_key, now_s);
}

pqxx::result execute_find_user_by_username(pqxx::read_transaction& tx, std::string_view username) {
    return tx.exec_params(std::string(kFindUserByUsernameSql), std::string(username));
}

pqxx::result execute_find_user_by_id(pqxx::read_transaction& tx, std::int64_t user_id) {
    return tx.exec_params(std::string(kFindUserByIdSql), user_id);
}

pqxx::result execute_update_user_password_hash(pqxx::work& tx, std::int64_t user_id,
                                               const PasswordHashValue& password_hash, std::int64_t token_version) {
    return tx.exec_params(std::string(kUpdateUserPasswordHashSql), user_id, password_hash.algorithm,
                          password_hash.iterations, password_hash.salt, password_hash.derived_key, token_version);
}

pqxx::result execute_list_users(pqxx::read_transaction& tx, std::int64_t limit, std::int64_t offset) {
    return tx.exec_params(std::string(kListUsersSql), limit + 1, offset);
}

pqxx::result execute_find_user_profile_for_update(pqxx::work& tx, std::int64_t user_id) {
    return tx.exec_params(std::string(kFindUserProfileForUpdateSql), user_id);
}

std::int64_t execute_count_active_owners(pqxx::work& tx) {
    return tx.exec1(std::string(kCountActiveOwnersSql))[0].as<std::int64_t>(0);
}

pqxx::result execute_update_user(pqxx::work& tx, std::int64_t user_id, UserRole role, UserStatus status) {
    return tx.exec_params(std::string(kUpdateUserRoleAndStatusSql), user_id, std::string(to_string(role)),
                          std::string(to_string(status)));
}

}  // namespace nebula::auth
