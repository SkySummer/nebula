#include "nebula/storage/repository/schema_executor.hpp"

#include <string>

#include "nebula/storage/repository/schema_sql.hpp"
#include "nebula/storage/repository/types.hpp"

namespace nebula::storage {

pqxx::row execute_check_storage_repository_schema_ready(pqxx::read_transaction& tx) {
    return tx.exec1(std::string(kCheckStorageRepositorySchemaReadySql));
}

void execute_acquire_global_storage_object_gc_exclusive_advisory_lock(pqxx::work& tx) {
    tx.exec_params(std::string(kAcquireExclusiveAdvisoryXactLockSql), kGlobalStorageObjectGcAdvisoryLockKey);
}

void execute_acquire_global_storage_object_gc_shared_advisory_lock(pqxx::work& tx) {
    tx.exec_params(std::string(kAcquireSharedAdvisoryXactLockSql), kGlobalStorageObjectGcAdvisoryLockKey);
}

void execute_acquire_storage_tree_advisory_lock(pqxx::work& tx, std::int64_t user_id) {
    tx.exec_params(std::string(kAcquireHashedTextAdvisoryXactLockSql), std::format("storage_tree:/users/{}", user_id));
}

void execute_acquire_storage_temp_path_advisory_lock(pqxx::work& tx, std::string_view temp_rel_path) {
    tx.exec_params(std::string(kAcquireHashedTextAdvisoryXactLockSql), std::format("storage_temp:{}", temp_rel_path));
}

}  // namespace nebula::storage
