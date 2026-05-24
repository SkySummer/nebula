#include "nebula/storage/repository/gc_executor.hpp"

#include <string>

#include "nebula/storage/repository/gc_sql.hpp"

namespace nebula::storage {

pqxx::result execute_find_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s) {
    return tx.exec_params(std::string(kFindExpiredUploadSessionsSql), expired_before_s);
}

void execute_delete_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s) {
    tx.exec_params(std::string(kDeleteExpiredUploadSessionsSql), expired_before_s);
}

pqxx::result execute_delete_expired_download_tickets(pqxx::work& tx, std::int64_t now_s) {
    return tx.exec_params(std::string(kDeleteExpiredDownloadTicketsSql), now_s);
}

pqxx::result execute_list_active_temp_rel_paths(pqxx::work& tx) {
    return tx.exec(std::string(kListActiveTempRelPathsSql));
}

void execute_sync_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s) {
    tx.exec_params(std::string(kSyncStorageObjectRefCountsSql), now_s);
}

void execute_reset_orphan_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s) {
    tx.exec_params(std::string(kResetOrphanStorageObjectRefCountsSql), now_s);
}

pqxx::result execute_list_object_rel_paths(pqxx::work& tx) {
    return tx.exec(std::string(kListObjectRelPathsSql));
}

pqxx::result execute_list_orphan_storage_objects(pqxx::work& tx) {
    return tx.exec(std::string(kListOrphanStorageObjectsSql));
}

pqxx::result execute_find_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256) {
    return tx.exec_params(std::string(kFindCleanupCandidateObjectSql), std::string(sha256));
}

void execute_delete_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256) {
    tx.exec_params(std::string(kDeleteCleanupCandidateObjectSql), std::string(sha256));
}

pqxx::result execute_find_storage_object_by_rel_path(pqxx::work& tx, std::string_view object_rel_path) {
    return tx.exec_params(std::string(kFindStorageObjectByRelPathSql), std::string(object_rel_path));
}

pqxx::result execute_find_upload_session_by_temp_rel_path(pqxx::work& tx, std::string_view temp_rel_path) {
    return tx.exec_params(std::string(kFindUploadSessionByTempRelPathSql), std::string(temp_rel_path));
}

pqxx::result execute_find_upload_failure_object_reference(pqxx::work& tx, std::string_view sha256) {
    return tx.exec_params(std::string(kFindUploadFailureObjectReferenceSql), std::string(sha256));
}

}  // namespace nebula::storage
