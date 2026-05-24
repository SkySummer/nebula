#ifndef NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_SQL_HPP
#define NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_SQL_HPP

#include <string_view>

namespace nebula::storage {

inline constexpr std::string_view kStoreDownloadTicketSql = R"sql(
INSERT INTO storage_download_tickets(ticket, user_id, canonical_path, created_at_s, expires_at_s)
VALUES($1, $2::bigint, $3, $4::bigint, $5::bigint)
ON CONFLICT (ticket) DO NOTHING
RETURNING ticket
)sql";

inline constexpr std::string_view kFindDownloadTicketSql = R"sql(
SELECT user_id,
       canonical_path,
       expires_at_s
FROM storage_download_tickets
WHERE ticket = $1
LIMIT 1
)sql";

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_DOWNLOAD_TICKETS_SQL_HPP
