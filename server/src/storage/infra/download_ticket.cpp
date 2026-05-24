#include "nebula/storage/infra/download_ticket.hpp"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/common/platform/time.hpp"
#include "nebula/common/security/crypto.hpp"

namespace nebula::storage {

std::string_view to_string(DownloadTicketIssueError error) noexcept {
    switch (error) {
        case DownloadTicketIssueError::InvalidTtl:
            return "invalid_ttl";
        case DownloadTicketIssueError::InvalidUserId:
            return "invalid_user_id";
        case DownloadTicketIssueError::InvalidPath:
            return "invalid_path";
        case DownloadTicketIssueError::ExpiryOverflow:
            return "expiry_overflow";
        case DownloadTicketIssueError::GenerateFailed:
            return "generate_failed";
    }
    std::unreachable();
}

std::expected<IssuedDownloadTicket, DownloadTicketIssueError> issue_download_ticket(const std::chrono::seconds ttl,
                                                                                    const std::int64_t user_id,
                                                                                    std::string_view canonical_path) {
    if (ttl <= std::chrono::seconds::zero()) {
        return std::unexpected(DownloadTicketIssueError::InvalidTtl);
    }
    if (user_id <= 0) {
        return std::unexpected(DownloadTicketIssueError::InvalidUserId);
    }
    if (canonical_path.empty() || canonical_path == "/") {
        return std::unexpected(DownloadTicketIssueError::InvalidPath);
    }

    const std::chrono::seconds now_s = common::now_epoch_seconds();
    if (now_s > std::chrono::seconds::max() - ttl) {
        return std::unexpected(DownloadTicketIssueError::ExpiryOverflow);
    }

    auto ticket = common::generate_random_hex_token_128();
    if (!ticket.has_value()) {
        return std::unexpected(DownloadTicketIssueError::GenerateFailed);
    }

    return IssuedDownloadTicket{
        .ticket = std::move(*ticket),
        .expires_at_s = (now_s + ttl).count(),
    };
}

DownloadTicketVerifyStatus verify_download_ticket(std::string_view ticket, const DownloadTicketClaims& claims) {
    if (!common::is_valid_random_hex_token_128(ticket)) {
        return DownloadTicketVerifyStatus::Invalid;
    }
    if (claims.user_id <= 0 || claims.canonical_path.empty() || claims.canonical_path == "/") {
        return DownloadTicketVerifyStatus::InternalError;
    }
    if (common::now_epoch_seconds().count() >= claims.expires_at_s) {
        return DownloadTicketVerifyStatus::Expired;
    }
    return DownloadTicketVerifyStatus::Valid;
}

}  // namespace nebula::storage
