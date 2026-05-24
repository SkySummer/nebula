#ifndef NEBULA_STORAGE_INFRA_DOWNLOAD_TICKET_HPP
#define NEBULA_STORAGE_INFRA_DOWNLOAD_TICKET_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace nebula::storage {

enum class DownloadTicketIssueError : std::uint8_t {
    InvalidTtl,
    InvalidUserId,
    InvalidPath,
    ExpiryOverflow,
    GenerateFailed,
};

enum class DownloadTicketVerifyStatus : std::uint8_t {
    Valid,
    Invalid,
    Expired,
    InternalError,
};

struct DownloadTicketClaims {
    std::int64_t user_id = 0;
    std::int64_t expires_at_s = 0;
    std::string canonical_path;
};

struct IssuedDownloadTicket {
    std::string ticket;
    std::int64_t expires_at_s = 0;
};

[[nodiscard]] std::string_view to_string(DownloadTicketIssueError error) noexcept;

[[nodiscard]] std::expected<IssuedDownloadTicket, DownloadTicketIssueError> issue_download_ticket(
    std::chrono::seconds ttl, std::int64_t user_id, std::string_view canonical_path);

[[nodiscard]] DownloadTicketVerifyStatus verify_download_ticket(std::string_view ticket,
                                                                const DownloadTicketClaims& claims);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_INFRA_DOWNLOAD_TICKET_HPP
