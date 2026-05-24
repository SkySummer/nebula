#ifndef NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_EXECUTOR_HPP
#define NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_EXECUTOR_HPP

#include <cstdint>
#include <pqxx/pqxx>
#include <string_view>

namespace nebula::storage {

pqxx::result execute_store_download_ticket(pqxx::work& tx, std::string_view ticket, std::int64_t user_id,
                                           std::string_view canonical_path, std::int64_t now_s,
                                           std::int64_t expires_at_s);

pqxx::result execute_find_download_ticket(pqxx::read_transaction& tx, std::string_view ticket);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_EXECUTOR_HPP
