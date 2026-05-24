#ifndef NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_EXECUTOR_HPP
#define NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

#include "nebula/storage/domain/types.hpp"

namespace nebula::storage {

pqxx::result execute_create_upload_session(pqxx::work& tx, const UploadSessionRecord& session, std::int64_t now_s);

pqxx::result execute_find_upload_session_for_append(pqxx::work& tx, std::string_view upload_id);

pqxx::result execute_advance_upload_session(pqxx::work& tx, std::string_view upload_id, std::int64_t now_s,
                                            std::int64_t new_size_bytes);

pqxx::result execute_find_upload_session_for_prepare(pqxx::read_transaction& tx, std::string_view upload_id);

pqxx::result execute_find_upload_session_for_finalize(pqxx::work& tx, std::string_view upload_id);

void execute_delete_upload_session(pqxx::work& tx, std::string_view upload_id);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_UPLOAD_SESSIONS_EXECUTOR_HPP
