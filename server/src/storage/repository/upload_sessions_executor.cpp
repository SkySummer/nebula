#include "nebula/storage/repository/upload_sessions_executor.hpp"

#include "nebula/storage/repository/upload_sessions_sql.hpp"

namespace nebula::storage {

pqxx::result execute_create_upload_session(pqxx::work& tx, const UploadSessionRecord& session, std::int64_t now_s) {
    return tx.exec(kCreateUploadSessionSql, pqxx::params{tx, session.upload_id, session.path, session.temp_rel_path,
                                                         session.total_chunks, session.temp_size_bytes, now_s});
}

pqxx::result execute_find_upload_session_for_append(pqxx::work& tx, std::string_view upload_id) {
    return tx.exec(kFindUploadSessionForAppendSql, pqxx::params{tx, upload_id});
}

pqxx::result execute_advance_upload_session(pqxx::work& tx, std::string_view upload_id, std::int64_t now_s,
                                            std::int64_t new_size_bytes) {
    return tx.exec(kAdvanceUploadSessionSql, pqxx::params{tx, upload_id, now_s, new_size_bytes});
}

pqxx::result execute_find_upload_session_for_prepare(pqxx::read_transaction& tx, std::string_view upload_id) {
    return tx.exec(kFindUploadSessionForPrepareSql, pqxx::params{tx, upload_id});
}

pqxx::result execute_find_upload_session_for_finalize(pqxx::work& tx, std::string_view upload_id) {
    return tx.exec(kFindUploadSessionForFinalizeSql, pqxx::params{tx, upload_id});
}

void execute_delete_upload_session(pqxx::work& tx, std::string_view upload_id) {
    tx.exec(kDeleteUploadSessionSql, pqxx::params{tx, upload_id}).no_rows();
}

}  // namespace nebula::storage
