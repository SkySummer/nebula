#include "nebula/storage/infra/download_ticket.hpp"

#include <chrono>
#include <string>

#include "nebula/common/platform/time.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_issue_download_ticket_populates_expiry_and_token() {
    const std::int64_t before_issue = nebula::common::now_epoch_seconds().count();
    const auto ticket = nebula::storage::issue_download_ticket(std::chrono::seconds{120}, 42, "/docs/readme.txt");
    const std::int64_t after_issue = nebula::common::now_epoch_seconds().count();

    test::expect_true(ticket.has_value(), "issue download ticket should generate token");
    test::expect_true(ticket->expires_at_s >= before_issue + 120, "issued download ticket should not expire too early");
    test::expect_true(ticket->expires_at_s <= after_issue + 120, "issued download ticket should not expire too late");
    test::expect_equal(ticket->ticket.size(), std::size_t{32}, "download ticket should use fixed random length");
}

void test_issue_download_ticket_rejects_invalid_claims() {
    const auto ticket = nebula::storage::issue_download_ticket(std::chrono::seconds{120}, 0, "/docs/readme.txt");
    test::expect_true(!ticket.has_value(), "issue download ticket should reject invalid claims");
    test::expect_equal(ticket.error(), storage::DownloadTicketIssueError::InvalidUserId,
                       "issue download ticket should return invalid user id error");
}

void test_issue_download_ticket_rejects_invalid_path() {
    const auto ticket = nebula::storage::issue_download_ticket(std::chrono::seconds{120}, 7, "/");
    test::expect_true(!ticket.has_value(), "issue download ticket should reject invalid path");
    test::expect_equal(ticket.error(), storage::DownloadTicketIssueError::InvalidPath,
                       "issue download ticket should return invalid path error");
}

void test_verify_download_ticket_accepts_valid_claims() {
    const storage::DownloadTicketClaims claims{
        .user_id = 7,
        .expires_at_s = nebula::common::now_epoch_seconds().count() + 60,
        .canonical_path = "/files/report.pdf",
    };

    const storage::DownloadTicketVerifyStatus status =
        nebula::storage::verify_download_ticket("0123456789abcdef0123456789abcdef", claims);
    test::expect_equal(status, storage::DownloadTicketVerifyStatus::Valid,
                       "verify download ticket should accept valid claims");
}

void test_verify_download_ticket_rejects_expired_claims() {
    const storage::DownloadTicketClaims claims{
        .user_id = 7,
        .expires_at_s = nebula::common::now_epoch_seconds().count() - 1,
        .canonical_path = "/files/report.pdf",
    };

    const storage::DownloadTicketVerifyStatus status =
        nebula::storage::verify_download_ticket("0123456789abcdef0123456789abcdef", claims);
    test::expect_equal(status, storage::DownloadTicketVerifyStatus::Expired,
                       "verify download ticket should report expired claims");
}

void test_verify_download_ticket_rejects_invalid_token_format() {
    const storage::DownloadTicketClaims claims{
        .user_id = 7,
        .expires_at_s = nebula::common::now_epoch_seconds().count() + 60,
        .canonical_path = "/files/report.pdf",
    };

    const storage::DownloadTicketVerifyStatus status = nebula::storage::verify_download_ticket("not-a-ticket", claims);
    test::expect_equal(status, storage::DownloadTicketVerifyStatus::Invalid,
                       "verify download ticket should reject invalid token");
}

void test_verify_download_ticket_rejects_invalid_claims() {
    const storage::DownloadTicketClaims claims{
        .user_id = 7,
        .expires_at_s = nebula::common::now_epoch_seconds().count() + 60,
        .canonical_path = "/",
    };

    const storage::DownloadTicketVerifyStatus status =
        nebula::storage::verify_download_ticket("0123456789abcdef0123456789abcdef", claims);
    test::expect_equal(status, storage::DownloadTicketVerifyStatus::InternalError,
                       "verify download ticket should reject invalid stored claims");
}

int run_download_ticket_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"issue download ticket populates expiry and token", test_issue_download_ticket_populates_expiry_and_token},
        {"issue download ticket rejects invalid claims", test_issue_download_ticket_rejects_invalid_claims},
        {"issue download ticket rejects invalid path", test_issue_download_ticket_rejects_invalid_path},
        {"verify download ticket accepts valid claims", test_verify_download_ticket_accepts_valid_claims},
        {"verify download ticket rejects expired claims", test_verify_download_ticket_rejects_expired_claims},
        {"verify download ticket rejects invalid token format",
         test_verify_download_ticket_rejects_invalid_token_format},
        {"verify download ticket rejects invalid claims", test_verify_download_ticket_rejects_invalid_claims},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_download_ticket_tests);
}
