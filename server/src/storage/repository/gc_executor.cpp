#include "nebula/storage/repository/gc_executor.hpp"

#include "nebula/storage/repository/gc_sql.hpp"

namespace nebula::storage {

pqxx::result execute_find_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s) {
    return tx.exec(kFindExpiredUploadSessionsSql, pqxx::params{tx, expired_before_s});
}

void execute_delete_expired_upload_sessions(pqxx::work& tx, std::int64_t expired_before_s) {
    tx.exec(kDeleteExpiredUploadSessionsSql, pqxx::params{tx, expired_before_s}).no_rows();
}

pqxx::result execute_delete_expired_download_tickets(pqxx::work& tx, std::int64_t now_s) {
    return tx.exec(kDeleteExpiredDownloadTicketsSql, pqxx::params{tx, now_s});
}

pqxx::result execute_list_active_temp_rel_paths(pqxx::work& tx) {
    return tx.exec(kListActiveTempRelPathsSql);
}

void execute_sync_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s) {
    tx.exec(kSyncStorageObjectRefCountsSql, pqxx::params{tx, now_s}).no_rows();
}

void execute_reset_orphan_storage_object_ref_counts(pqxx::work& tx, std::int64_t now_s) {
    tx.exec(kResetOrphanStorageObjectRefCountsSql, pqxx::params{tx, now_s}).no_rows();
}

pqxx::result execute_list_object_rel_paths(pqxx::work& tx) {
    return tx.exec(kListObjectRelPathsSql);
}

pqxx::result execute_list_orphan_storage_objects(pqxx::work& tx) {
    return tx.exec(kListOrphanStorageObjectsSql);
}

pqxx::result execute_find_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256) {
    return tx.exec(kFindCleanupCandidateObjectSql, pqxx::params{tx, sha256});
}

void execute_delete_cleanup_candidate_object(pqxx::work& tx, std::string_view sha256) {
    tx.exec(kDeleteCleanupCandidateObjectSql, pqxx::params{tx, sha256}).no_rows();
}

pqxx::result execute_find_storage_object_by_rel_path(pqxx::work& tx, std::string_view object_rel_path) {
    return tx.exec(kFindStorageObjectByRelPathSql, pqxx::params{tx, object_rel_path});
}

pqxx::result execute_find_upload_session_by_temp_rel_path(pqxx::work& tx, std::string_view temp_rel_path) {
    return tx.exec(kFindUploadSessionByTempRelPathSql, pqxx::params{tx, temp_rel_path});
}

pqxx::result execute_find_upload_failure_object_reference(pqxx::work& tx, std::string_view sha256) {
    return tx.exec(kFindUploadFailureObjectReferenceSql, pqxx::params{tx, sha256});
}

}  // namespace nebula::storage
