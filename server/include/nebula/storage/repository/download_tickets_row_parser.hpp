#ifndef NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_ROW_PARSER_HPP
#define NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_ROW_PARSER_HPP

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

namespace nebula::storage {

struct DownloadTicketRow {
    std::int64_t user_id = 0;
    std::string canonical_path;
    std::int64_t expires_at_s = 0;
};

std::optional<DownloadTicketRow> parse_download_ticket_row(const pqxx::row& row);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_ROW_PARSER_HPP
