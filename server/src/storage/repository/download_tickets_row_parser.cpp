#include "nebula/storage/repository/download_tickets_row_parser.hpp"

#include "nebula/database/row_check.hpp"

namespace nebula::storage {

std::optional<DownloadTicketRow> parse_download_ticket_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 3) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    return DownloadTicketRow{
        .user_id = row[0].as<std::int64_t>(0),
        .canonical_path = row[1].as<std::string>(),
        .expires_at_s = row[2].as<std::int64_t>(0),
    };
}

}  // namespace nebula::storage
