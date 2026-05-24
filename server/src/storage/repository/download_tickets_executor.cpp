#include "nebula/storage/repository/download_tickets_executor.hpp"

#include <string>

#include "nebula/storage/repository/download_tickets_sql.hpp"

namespace nebula::storage {

pqxx::result execute_store_download_ticket(pqxx::work& tx, std::string_view ticket, std::int64_t user_id,
                                           std::string_view canonical_path, std::int64_t now_s,
                                           std::int64_t expires_at_s) {
    return tx.exec_params(std::string(kStoreDownloadTicketSql), std::string(ticket), user_id,
                          std::string(canonical_path), now_s, expires_at_s);
}

pqxx::result execute_find_download_ticket(pqxx::read_transaction& tx, std::string_view ticket) {
    return tx.exec_params(std::string(kFindDownloadTicketSql), std::string(ticket));
}

}  // namespace nebula::storage
