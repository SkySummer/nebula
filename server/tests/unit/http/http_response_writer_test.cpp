#include "nebula/http/http_response_writer.hpp"

#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::http::HttpResponse;
using nebula::http::HttpStatus;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_not_contains;
using nebula::testsupport::expect_true;

void test_reserved_headers_overridden_by_serializer() {
    HttpResponse response;
    response.status = HttpStatus::OK;
    response.body = "ok";
    response.headers.emplace("content-length", "999");
    response.headers.emplace("Connection", "close");
    response.headers.emplace("X-Trace-Id", "abc");

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "\r\nContent-Length: 2\r\n", "serializer should recompute content-length");
    expect_contains(serialized, "\r\nConnection: keep-alive\r\n", "serializer should set keep-alive");
    expect_contains(serialized, "\r\nX-Trace-Id: abc\r\n", "non-managed headers should be preserved");
    expect_not_contains(serialized, "\r\ncontent-length: 999\r\n", "manual content-length should be filtered");
    expect_not_contains(serialized, "\r\nConnection: close\r\n", "manual connection should be filtered");
}

void test_connection_close_overrides_user_header() {
    HttpResponse response;
    response.status = HttpStatus::OK;
    response.body = "pong";
    response.headers.emplace("connection", "keep-alive");

    const std::string serialized = nebula::http::serialize_http_response(response, false);
    expect_contains(serialized, "\r\nConnection: close\r\n", "serializer should force close");
    expect_not_contains(serialized, "\r\nconnection: keep-alive\r\n", "manual connection should not leak");
}

void test_plain_text_default_header_canonical() {
    const HttpResponse response = nebula::http::make_plain_text_response(HttpStatus::OK, "hi");
    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "\r\nContent-Type: text/plain; charset=utf-8\r\n", "content-type should be canonical");
    expect_not_contains(serialized, "\r\ncontent-type:", "lowercase content-type should not appear");
}

void test_redirect_response_sets_location_header() {
    const HttpResponse response = nebula::http::make_redirect_response(HttpStatus::Found, "/login");

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "HTTP/1.1 302 Found\r\n", "redirect response should keep requested status");
    expect_contains(serialized, "\r\nLocation: /login\r\n", "redirect response should include location header");
    expect_contains(serialized, "\r\nContent-Length: 0\r\n", "redirect response should default to empty body");
}

void test_header_value_with_crlf_is_filtered() {
    HttpResponse response;
    response.status = HttpStatus::OK;
    response.body = "ok";
    response.headers.emplace("X-Trace-Id", "safe\r\nInjected: yes");

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_not_contains(serialized, "\r\nX-Trace-Id: ", "header with CRLF in value should be dropped");
    expect_not_contains(serialized, "\r\nInjected: yes\r\n", "CRLF injection should not create extra header");
    expect_contains(serialized, "\r\nContent-Length: 2\r\n", "serializer should still emit content-length");
}

void test_header_key_with_crlf_is_filtered() {
    HttpResponse response;
    response.status = HttpStatus::OK;
    response.body = "ok";
    response.headers.emplace("X-Trace-Id\r\nInjected", "safe");

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_not_contains(serialized, "\r\nInjected: safe\r\n", "CRLF injection in key should be dropped");
    expect_not_contains(serialized, "\r\nX-Trace-Id", "invalid key should not be serialized");
    expect_contains(serialized, "\r\nConnection: keep-alive\r\n", "serializer should still emit managed headers");
}

void test_suppress_body_keeps_content_length() {
    HttpResponse response;
    response.status = HttpStatus::OK;
    response.body = "head-body";

    const std::string serialized = nebula::http::serialize_http_response(response, true, true);
    expect_contains(serialized, "\r\nContent-Length: 9\r\n", "serializer should keep content-length of original body");
    expect_not_contains(serialized, "head-body", "suppressed response should not include body bytes");
    expect_true(serialized.ends_with("\r\n\r\n"), "suppressed response should end after headers");
}

void test_informational_status_suppresses_body_automatically() {
    HttpResponse response;
    response.status = HttpStatus::EarlyHints;
    response.body = "hint-body";

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "HTTP/1.1 103 Early Hints\r\n", "status line should be preserved");
    expect_contains(serialized, "\r\nContent-Length: 0\r\n", "1xx response should force zero content-length");
    expect_not_contains(serialized, "hint-body", "1xx response should suppress body automatically");
    expect_true(serialized.ends_with("\r\n\r\n"), "1xx response should end after headers");
}

void test_no_content_status_suppresses_body_automatically() {
    HttpResponse response;
    response.status = HttpStatus::NoContent;
    response.body = "unexpected";

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "HTTP/1.1 204 No Content\r\n", "status line should be preserved");
    expect_contains(serialized, "\r\nContent-Length: 0\r\n", "204 response should force zero content-length");
    expect_not_contains(serialized, "unexpected", "204 response should suppress body automatically");
    expect_true(serialized.ends_with("\r\n\r\n"), "204 response should end after headers");
}

void test_not_modified_status_suppresses_body_automatically() {
    HttpResponse response;
    response.status = HttpStatus::NotModified;
    response.body = "cached";

    const std::string serialized = nebula::http::serialize_http_response(response, true);
    expect_contains(serialized, "HTTP/1.1 304 Not Modified\r\n", "status line should be preserved");
    expect_contains(serialized, "\r\nContent-Length: 0\r\n", "304 response should force zero content-length");
    expect_not_contains(serialized, "cached", "304 response should suppress body automatically");
    expect_true(serialized.ends_with("\r\n\r\n"), "304 response should end after headers");
}

int run_http_response_writer_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"reserved headers overridden by serializer", test_reserved_headers_overridden_by_serializer},
        {"connection close overrides user header", test_connection_close_overrides_user_header},
        {"plain text default header canonical", test_plain_text_default_header_canonical},
        {"redirect response sets location header", test_redirect_response_sets_location_header},
        {"header value with crlf is filtered", test_header_value_with_crlf_is_filtered},
        {"header key with crlf is filtered", test_header_key_with_crlf_is_filtered},
        {"suppress body keeps content length", test_suppress_body_keeps_content_length},
        {"1xx suppresses body automatically", test_informational_status_suppresses_body_automatically},
        {"204 suppresses body automatically", test_no_content_status_suppresses_body_automatically},
        {"304 suppresses body automatically", test_not_modified_status_suppresses_body_automatically},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_http_response_writer_tests);
}
