#ifndef NEBULA_STORAGE_REPOSITORY_GC_EXECUTOR_HPP
#define NEBULA_STORAGE_REPOSITORY_GC_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

namespace nebula::storage {

pqxx::result execute_find_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s);

void execute_delete_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s);

pqxx::result execute_delete_expired_download_tickets(pqxx::work& tx, std::int64_t now_s);

pqxx::result execute_list_active_temp_rel_paths(pqxx::work& tx);

void execute_sync_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s);

void execute_reset_orphan_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s);

pqxx::result execute_list_object_rel_paths(pqxx::work& tx);

pqxx::result execute_list_orphan_storage_objects(pqxx::work& tx);

pqxx::result execute_find_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256);

void execute_delete_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256);

pqxx::result execute_find_storage_object_by_rel_path(pqxx::work& tx, std::string_view object_rel_path);

pqxx::result execute_find_upload_session_by_temp_rel_path(pqxx::work& tx, std::string_view temp_rel_path);

pqxx::result execute_find_upload_failure_object_reference(pqxx::work& tx, std::string_view sha256);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_GC_EXECUTOR_HPP
