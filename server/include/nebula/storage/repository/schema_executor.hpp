#ifndef NEBULA_STORAGE_REPOSITORY_SCHEMA_EXECUTOR_HPP
#define NEBULA_STORAGE_REPOSITORY_SCHEMA_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

namespace nebula::storage {

pqxx::row execute_check_storage_repository_schema_ready(pqxx::read_transaction& tx);

void execute_acquire_global_storage_object_gc_exclusive_advisory_lock(pqxx::work& tx);

void execute_acquire_global_storage_object_gc_shared_advisory_lock(pqxx::work& tx);

void execute_acquire_storage_tree_advisory_lock(pqxx::work& tx, std::int64_t user_id);

void execute_acquire_storage_temp_path_advisory_lock(pqxx::work& tx, std::string_view temp_rel_path);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_SCHEMA_EXECUTOR_HPP
