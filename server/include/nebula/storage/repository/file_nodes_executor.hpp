#ifndef NEBULA_STORAGE_REPOSITORY_FILE_NODES_EXECUTOR_HPP
#define NEBULA_STORAGE_REPOSITORY_FILE_NODES_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

#include "nebula/storage/domain/types.hpp"

namespace nebula::storage {

void execute_ensure_user_root_directory(pqxx::work& tx, std::int64_t user_id, std::int64_t now_s);

pqxx::result execute_find_node_type_for_update(pqxx::work& tx, std::string_view path);

pqxx::result execute_find_storage_node_descendant(pqxx::work& tx, std::string_view path);

pqxx::result execute_find_user_quota_bytes(pqxx::transaction_base& tx, std::int64_t user_id);

pqxx::row execute_sum_user_file_bytes(pqxx::transaction_base& tx, std::int64_t user_id);

pqxx::result execute_find_existing_file_target_for_update(pqxx::work& tx, std::string_view path);

pqxx::result execute_find_file_node(pqxx::read_transaction& tx, std::string_view path);

void execute_insert_directory_node(pqxx::work& tx, std::string_view scoped_path, std::int64_t now_s);

pqxx::result execute_list_directory_children(pqxx::work& tx, std::string_view scoped_path,
                                             const DirectoryListOptions& options);

pqxx::result execute_list_recent_files(pqxx::work& tx, std::int64_t user_id, std::int64_t limit);

pqxx::result execute_list_storage_usage_files(pqxx::work& tx, std::int64_t user_id);

void execute_upsert_storage_object(pqxx::work& tx, std::string_view sha256, std::int64_t size_bytes,
                                   std::string_view object_rel_path, std::int64_t now_s);

void execute_insert_file_node(pqxx::work& tx, std::string_view path, std::string_view sha256, std::int64_t size_bytes,
                              std::int64_t now_s);

void execute_update_file_node_with_object(pqxx::work& tx, std::string_view path, std::string_view sha256,
                                          std::int64_t size_bytes, std::int64_t now_s);

void execute_update_file_node_size(pqxx::work& tx, std::string_view path, std::int64_t size_bytes, std::int64_t now_s);

pqxx::result execute_decrement_storage_object_ref_count(pqxx::work& tx, std::string_view sha256, std::int64_t now_s);

pqxx::result execute_find_node_for_delete(pqxx::work& tx, std::string_view scoped_path);

void execute_delete_node(pqxx::work& tx, std::string_view scoped_path);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_FILE_NODES_EXECUTOR_HPP
