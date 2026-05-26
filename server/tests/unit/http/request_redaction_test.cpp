#include "nebula/http/redaction/request_redaction.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_redact_request_path_keeps_non_sensitive_path() {
    test::expect_equal(nebula::http::redact_request_path("/healthz"), std::string("/healthz"),
                       "non-sensitive path should stay unchanged");
}

void test_redact_request_path_redacts_download_ticket() {
    test::expect_equal(nebula::http::redact_request_path("/api/storage/downloads/0123456789abcdef"),
                       std::string("/api/storage/downloads/{download_ticket}"),
                       "download ticket path should be redacted");
}

void test_redact_request_line_keeps_absolute_form_request() {
    test::expect_equal(nebula::http::redact_request_line("GET http://localhost/healthz HTTP/1.1"),
                       std::string("GET http://localhost/healthz HTTP/1.1"),
                       "non-sensitive absolute-form request line should stay unchanged");
}

void test_redact_request_line_redacts_origin_form_download_ticket() {
    test::expect_equal(
        nebula::http::redact_request_line("GET /api/storage/downloads/0123456789abcdef?download=1 HTTP/1.1"),
        std::string("GET /api/storage/downloads/{download_ticket}?download=1 HTTP/1.1"),
        "origin-form request line should redact download ticket");
}

void test_redact_request_line_redacts_absolute_form_download_ticket() {
    test::expect_equal(nebula::http::redact_request_line(
                           "GET http://localhost/api/storage/downloads/0123456789abcdef?download=1 HTTP/1.1"),
                       std::string("GET http://localhost/api/storage/downloads/{download_ticket}?download=1 HTTP/1.1"),
                       "absolute-form request line should redact download ticket");
}

int run_request_redaction_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"redact request path keeps non-sensitive path", test_redact_request_path_keeps_non_sensitive_path},
        {"redact request path redacts download ticket", test_redact_request_path_redacts_download_ticket},
        {"redact request line keeps absolute form request", test_redact_request_line_keeps_absolute_form_request},
        {"redact request line redacts origin form download ticket",
         test_redact_request_line_redacts_origin_form_download_ticket},
        {"redact request line redacts absolute form download ticket",
         test_redact_request_line_redacts_absolute_form_download_ticket},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_request_redaction_tests);
}
