#include "nebula/http/codec/parser.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "nebula/http/routing/path.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

constexpr std::size_t kMaxHeader = std::size_t{16} * 1024U;
constexpr std::size_t kMaxBody = std::size_t{1024} * 1024U;
constexpr std::size_t kMaxRequestTarget = std::numeric_limits<std::size_t>::max();

void expect_parse_error(const nebula::http::ParseResult& parsed, http::HttpStatus expected_status,
                        const char* message) {
    test::expect_equal(parsed.status, http::ParseStatus::Error, message);
    test::expect_equal(parsed.http_status, expected_status, message);
}

void expect_route_segments(const nebula::http::ParseResult& parsed, const std::vector<std::string>& expected,
                           const char* message) {
    test::expect_equal(nebula::http::split_http_path_segments(parsed.request.path), expected, message);
}

void test_parse_get_request() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "GET request should parse");
    test::expect_equal(parsed.request.method, http::HttpMethod::Get, "method should be GET");
    test::expect_equal(parsed.request.request_line, std::string("GET /healthz HTTP/1.1"),
                       "request line should preserve raw text");
    test::expect_equal(parsed.request.path, std::string("/healthz"), "path should parse");
    expect_route_segments(parsed, {"", "healthz"}, "route path should split from normalized path");
    test::expect_true(parsed.request.keep_alive, "http/1.1 should keep alive by default");
}

void test_parse_get_request_with_query_uses_path_only_for_routing() {
    const std::string raw = "GET /healthz?ready=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "GET with query should parse");
    test::expect_equal(parsed.request.path, std::string("/healthz"), "route path should not include query");
    test::expect_equal(parsed.request.query_params.size(), std::size_t{1}, "query should expose one kv pair");
    test::expect_equal(parsed.request.query_params.at("ready"), std::vector<std::string>{"1"},
                       "query value should be preserved");
    expect_route_segments(parsed, {"", "healthz"}, "query should not appear in route path segments");
}

void test_parse_get_request_with_query_keeps_kv_shape_after_fragment() {
    const std::string raw = "GET /files?sort=name&desc#preview HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "GET with query and fragment should parse");
    test::expect_equal(parsed.request.path, std::string("/files"), "fragment should stay out of normalized path");
    test::expect_equal(parsed.request.query_params.size(), std::size_t{2}, "query parser should expose both kv pairs");
    test::expect_equal(parsed.request.query_params.at("sort"), std::vector<std::string>{"name"},
                       "sort param should parse");
    test::expect_equal(parsed.request.query_params.at("desc"), std::vector<std::string>{""},
                       "param without equals should map to empty value");
}

void test_parse_get_request_with_duplicate_query_key_preserves_all_values() {
    const std::string raw = "GET /files?sort=name&sort=time&sort=size HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "GET with duplicate query key should parse");
    test::expect_equal(parsed.request.path, std::string("/files"), "duplicate query key should not affect path");
    test::expect_equal(parsed.request.query_params.size(), std::size_t{1},
                       "duplicate key should stay grouped under one map entry");
    test::expect_equal(parsed.request.query_params.at("sort"), std::vector<std::string>({"name", "time", "size"}),
                       "duplicate query key should preserve all values in order");
}

void test_parse_path_segments_root() {
    const std::string raw = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "root path should parse");
    expect_route_segments(parsed, {"", ""}, "root path should keep leading and trailing empty segments");
}

void test_parse_path_segments_trailing_slash() {
    const std::string raw = "GET /users/42/ HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "path with trailing slash should parse");
    expect_route_segments(parsed, {"", "users", "42", ""}, "trailing slash should be preserved as an empty segment");
}

void test_parse_path_segments_repeated_slash() {
    const std::string raw = "GET /users//42 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "path with repeated slash should parse");
    expect_route_segments(parsed, {"", "users", "", "42"}, "repeated slash should be preserved as an empty segment");
}

void test_parse_post_with_body() {
    const std::string body = R"({"ok":"yes"})";
    const std::string raw = std::format(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\nContent-Type: application/json\r\n\r\n{}",
        body.size(), body);
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "POST should parse");
    test::expect_equal(parsed.request.method, http::HttpMethod::Post, "method should be POST");
    test::expect_equal(parsed.request.body, body, "body should parse");
}

void test_parse_need_more_body() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n12";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::NeedMore, "partial body should need more data");
}

void test_parse_bad_request_line() {
    const std::string raw = "BROKEN\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid request line should fail");
}

void test_parse_unsupported_method() {
    const std::string raw = "FOO /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::NotImplemented, "unknown method should be rejected");
}

void test_parse_unsupported_http_version() {
    const std::string raw = "GET /healthz HTTP/2\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::HTTPVersionNotSupported, "unsupported version should be rejected");
}

void test_parse_invalid_http_version() {
    const std::string raw = "GET /healthz HTTP/2.beta\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid version format should be bad request");
}

void test_parse_invalid_header() {
    const std::string raw = "GET / HTTP/1.1\r\nInvalidHeader\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid header should fail");
}

void test_parse_invalid_header_key_char() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nBad(Header): value\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "header key with non-token char should fail");
}

void test_parse_header_key_space_before_colon_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost : localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "space before colon in header key should fail");
}

void test_parse_header_key_token_chars_allowed() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Test_Header~1: ok\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "header key token chars should parse");
    test::expect_true(parsed.request.headers.contains("x-test_header~1"),
                      "header key should be normalized to lowercase");
}

void test_parse_header_value_with_ctl_rejected() {
    const std::string raw = std::format("GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Test: ok{}\r\n\r\n", '\x01');
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "header value with ctl char should fail");
}

void test_parse_header_value_with_del_rejected() {
    const std::string raw = std::format("GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Test: ok{}\r\n\r\n", '\x7F');
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "header value with del char should fail");
}

void test_parse_invalid_content_length() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: abc\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid content-length should fail");
}

void test_parse_content_too_large() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\n123456";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, 5U, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::ContentTooLarge, "oversized body should fail");
}

void test_parse_content_length_sum_overflow_rejected() {
    const std::string raw = std::format("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n",
                                        std::numeric_limits<std::size_t>::max());
    const auto parsed =
        nebula::http::parse_http_request(raw, kMaxHeader, std::numeric_limits<std::size_t>::max(), kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::ContentTooLarge, "content-length sum overflow should fail");
}

void test_parse_header_too_large() {
    const std::string raw(20000U, 'a');
    const auto parsed = nebula::http::parse_http_request(raw, 1024U, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::RequestHeaderFieldsTooLarge, "oversized header should fail");
}

void test_parse_uri_too_long() {
    const std::string raw = "GET /0123456789 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, 8U);
    expect_parse_error(parsed, http::HttpStatus::URITooLong, "oversized request-target should fail");
}

void test_parse_options_asterisk_form() {
    const std::string raw = "OPTIONS * HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "OPTIONS * should parse");
    test::expect_equal(parsed.request.method, http::HttpMethod::Options, "method should be OPTIONS");
    test::expect_equal(parsed.request.path, std::string("*"), "asterisk-form should preserve path");
}

void test_parse_get_asterisk_form_rejected() {
    const std::string raw = "GET * HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "GET * should be rejected");
}

void test_parse_post_asterisk_form_rejected() {
    const std::string raw = "POST * HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "POST * should be rejected");
}

void test_parse_origin_form_without_leading_slash_rejected() {
    const std::string raw = "GET healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "origin-form without leading slash should be rejected");
}

void test_parse_connect_requires_authority_form() {
    const std::string raw = "CONNECT /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "CONNECT with non-authority-form should be rejected");
}

void test_parse_connection_close() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "request should parse");
    test::expect_true(!parsed.request.keep_alive, "connection close should disable keep-alive");
}

void test_parse_connection_close_in_token_list() {
    const std::string raw =
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive, close, upgrade\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "request with connection token list should parse");
    test::expect_true(!parsed.request.keep_alive, "connection token list containing close should disable keep-alive");
}

void test_parse_http10_connection_keep_alive_in_token_list() {
    const std::string raw = "GET /healthz HTTP/1.0\r\nHost: localhost\r\nConnection: upgrade, keep-alive\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "http/1.0 request with connection token list should parse");
    test::expect_true(parsed.request.keep_alive,
                      "http/1.0 connection token list containing keep-alive should enable keep-alive");
}

void test_parse_duplicate_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost\r\nHost: duplicate\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "duplicate header should fail");
}

void test_parse_duplicate_content_length_same_value() {
    const std::string body = "ok";
    const std::string raw = std::format(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 02\r\n\r\n{}", body);
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "same content-length should parse");
    test::expect_equal(parsed.request.body, body, "body should parse when content-length matches");
}

void test_parse_duplicate_content_length_conflict() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nok";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "conflicting content-length should fail");
}

void test_parse_chunked_body() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "chunked request should parse");
    test::expect_equal(parsed.request.body, std::string("hello world"), "chunked body should decode");
}

void test_parse_chunked_body_with_extension_and_trailer() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "a;foo=bar\r\n0123456789\r\n0\r\nX-Trace: 1\r\nX-Tag: ok\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "chunked request with extension and trailer should parse");
    test::expect_equal(parsed.request.body, std::string("0123456789"),
                       "chunked body should decode with trailer present");
    test::expect_true(!parsed.request.headers.contains("x-trace"),
                      "chunk trailer should not be exposed as request header");
}

void test_parse_chunked_need_more() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n0\r\nX-T: ok\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::NeedMore,
                       "incomplete chunk trailer terminator should need more data");
}

void test_parse_chunked_incremental_context_across_fragments() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n";
    const std::vector<std::size_t> fragment_sizes = {3U, 7U, 11U, 13U, 5U, 17U, raw.size()};

    nebula::http::HttpRequestParseContext context;
    std::string partial;
    std::size_t cursor = 0;
    for (std::size_t idx = 0; idx < fragment_sizes.size(); ++idx) {
        const std::size_t next_cursor = std::min(raw.size(), cursor + fragment_sizes[idx]);
        partial.append(raw.substr(cursor, next_cursor - cursor));
        cursor = next_cursor;

        const auto parsed = nebula::http::parse_http_request(partial, kMaxHeader, kMaxBody,
                                                             std::numeric_limits<std::size_t>::max(), context);
        if (idx + 1U < fragment_sizes.size()) {
            test::expect_equal(parsed.status, http::ParseStatus::NeedMore,
                               "chunked incremental parsing should keep requesting more data");
            continue;
        }

        test::expect_equal(parsed.status, http::ParseStatus::Complete, "chunked incremental parsing should complete");
        test::expect_equal(parsed.request.body, std::string("abc"), "chunked incremental parsing should decode once");
    }
}

void test_parse_context_resets_after_complete_for_pipeline() {
    const std::string first =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    const std::string second = "GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string combined = std::format("{}{}", first, second);

    nebula::http::HttpRequestParseContext context;
    const auto first_parsed = nebula::http::parse_http_request(combined, kMaxHeader, kMaxBody,
                                                               std::numeric_limits<std::size_t>::max(), context);
    test::expect_equal(first_parsed.status, http::ParseStatus::Complete,
                       "first request should parse with incremental context");
    test::expect_equal(first_parsed.request.body, std::string("hello"), "first request body should decode");
    test::expect_equal(first_parsed.consumed_bytes, first.size(), "first request should consume only first message");

    const auto second_view = std::string_view(combined).substr(first_parsed.consumed_bytes);
    const auto second_parsed = nebula::http::parse_http_request(second_view, kMaxHeader, kMaxBody,
                                                                std::numeric_limits<std::size_t>::max(), context);
    test::expect_equal(second_parsed.status, http::ParseStatus::Complete,
                       "context should reset automatically for pipelined request");
    test::expect_equal(second_parsed.request.method, http::HttpMethod::Get, "second request method should parse");
    test::expect_equal(second_parsed.request.path, std::string("/next"), "second request path should parse");
}

void test_parse_chunked_context_releases_decoded_capacity_after_complete() {
    const std::string body(std::size_t{64} * 1024U, 'x');
    const std::string raw = std::format(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n{:x}\r\n{}\r\n0\r\n\r\n",
        body.size(), body);

    nebula::http::HttpRequestParseContext context;
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget, context);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "large chunked request should parse");
    test::expect_equal(parsed.request.body, body, "large chunked body should decode");
    test::expect_true(context.decoded_chunked_body.capacity() < body.size(),
                      "decoded chunked buffer capacity should be released after complete");
}

void test_parse_context_resets_after_error_for_reuse() {
    const std::string broken = "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n";
    const std::string next = "GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n";

    nebula::http::HttpRequestParseContext context;
    const auto broken_parsed =
        nebula::http::parse_http_request(broken, kMaxHeader, kMaxBody, kMaxRequestTarget, context);
    expect_parse_error(broken_parsed, http::HttpStatus::BadRequest, "invalid chunk size should return parse error");
    test::expect_true(!context.header_parsed, "context should reset header parsed flag after error");
    test::expect_true(!context.chunked_body, "context should reset chunked flag after error");
    test::expect_equal(context.header_bytes, 0U, "context should reset header bytes after error");
    test::expect_equal(context.chunk_cursor, 0U, "context should reset chunk cursor after error");
    test::expect_equal(context.chunk_size, 0U, "context should reset chunk size after error");
    test::expect_equal(context.chunk_phase, nebula::http::ChunkedDecodePhase::ChunkSizeLine,
                       "context should reset chunk decode phase after error");

    const auto next_parsed = nebula::http::parse_http_request(next, kMaxHeader, kMaxBody, kMaxRequestTarget, context);
    test::expect_equal(next_parsed.status, http::ParseStatus::Complete,
                       "context should be reusable right after parse error");
    test::expect_equal(next_parsed.request.method, http::HttpMethod::Get, "request method should parse after reset");
    test::expect_equal(next_parsed.request.path, std::string("/next"), "request path should parse after reset");
}

void test_parse_chunked_with_content_length_rejected() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "content-length with transfer-encoding should fail");
}

void test_parse_transfer_encoding_unsupported() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::NotImplemented, "unsupported transfer-encoding should return 501");
}

void test_parse_invalid_chunked_body() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid chunk-size should fail");
}

void test_parse_chunked_size_with_leading_whitespace_rejected() {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n a\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "chunk-size with leading whitespace should fail");
}

void test_parse_chunked_size_with_whitespace_before_extension_rejected() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\na ;foo=bar\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "chunk-size with whitespace before extension should fail");
}

void test_parse_chunked_content_too_large() {
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n6\r\n123456\r\n0\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, 5U, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::ContentTooLarge,
                       "decoded chunked payload exceeding limit should fail");
}

void test_parse_chunked_consumed_bytes_for_pipeline() {
    const std::string first =
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    const std::string second = "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string raw = std::format("{}{}", first, second);

    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "chunked request should parse before pipelined request");
    test::expect_equal(parsed.request.body, std::string("hello"), "chunked request body should decode");
    test::expect_equal(parsed.consumed_bytes, first.size(), "consumed bytes should include full chunked request bytes");
}

void test_parse_consumed_bytes_for_pipeline() {
    const std::string first = "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string second = "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string raw = std::format("{}{}", first, second);

    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "first request should parse");
    test::expect_equal(parsed.consumed_bytes, first.size(), "consumed bytes should equal first request size");
}

void test_parse_http11_missing_host_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nConnection: close\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "http/1.1 without host should fail");
}

void test_parse_http10_without_host_allowed() {
    const std::string raw = "GET /healthz HTTP/1.0\r\nConnection: close\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "http/1.0 without host should parse");
}

void test_parse_empty_host_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost:\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "empty host should be rejected");
}

void test_parse_invalid_host_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: bad host\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "host with spaces should fail");
}

void test_parse_invalid_host_port_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost:abc\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "host with invalid port should fail");
}

void test_parse_valid_ipv4_host_header() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "valid ipv4 host should parse");
}

void test_parse_invalid_ipv4_host_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: 256.0.0.1:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid ipv4 host should fail");
}

void test_parse_ipv4_with_leading_zero_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: 127.00.0.1:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "ipv4 host with leading zero should fail");
}

void test_parse_valid_ipv6_host_header() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: [2001:db8::1]:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "valid ipv6 host should parse");
}

void test_parse_invalid_ipv6_host_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: [2001:::1]:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid ipv6 host should fail");
}

void test_parse_out_of_range_host_port_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: localhost:65536\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "host with out-of-range port should fail");
}

void test_parse_valid_ipvfuture_host_header() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: [v1.fe80::a]:8080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "valid ipvfuture host should parse");
}

void test_parse_invalid_ipvfuture_host_header_rejected() {
    const std::string raw = "GET /healthz HTTP/1.1\r\nHost: [v1]\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "invalid ipvfuture host should fail");
}

void test_parse_absolute_form_host_match() {
    const std::string raw = "GET http://example.com/healthz HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "absolute-form host should match authority");
    test::expect_equal(parsed.request.path, std::string("/healthz"), "absolute-form should normalize path for routing");
}

void test_parse_absolute_form_without_path_normalized_to_root() {
    const std::string raw = "GET http://example.com HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "absolute-form without path should parse");
    test::expect_equal(parsed.request.path, std::string("/"), "absolute-form without path should normalize to root");
}

void test_parse_absolute_form_keeps_original_request_line() {
    const std::string raw = "GET http://example.com/healthz?ready=1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);

    test::expect_equal(parsed.status, http::ParseStatus::Complete, "absolute-form should parse");
    test::expect_equal(parsed.request.request_line, std::string("GET http://example.com/healthz?ready=1 HTTP/1.1"),
                       "request line should keep original absolute-form");
    test::expect_equal(parsed.request.path, std::string("/healthz"), "route path should exclude absolute-form query");
    test::expect_equal(parsed.request.query_params.at("ready"), std::vector<std::string>{"1"},
                       "absolute-form query should be exposed as kv");
    expect_route_segments(parsed, {"", "healthz"}, "absolute-form should split normalized path segments");
}

void test_parse_absolute_form_query_only_normalized_with_root() {
    const std::string raw = "GET http://example.com?ready=1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "absolute-form query-only should parse");
    test::expect_equal(parsed.request.path, std::string("/"), "query-only absolute-form should normalize to root path");
    test::expect_equal(parsed.request.query_params.at("ready"), std::vector<std::string>{"1"},
                       "query-only absolute-form should expose query kv");
}

void test_parse_absolute_form_host_case_insensitive_match() {
    const std::string raw = "GET http://example.com/healthz HTTP/1.1\r\nHost: EXAMPLE.COM\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "absolute-form host compare should be case-insensitive");
}

void test_parse_absolute_form_ipv6_compressed_equivalent_match() {
    const std::string raw = "GET http://[2001:0db8:0:0:0:0:0:1]/healthz HTTP/1.1\r\nHost: [2001:db8::1]\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "absolute-form ipv6 equivalent forms should match");
}

void test_parse_absolute_form_ipv6_port_numeric_equivalent_match() {
    const std::string raw = "GET http://[2001:db8::1]:80/healthz HTTP/1.1\r\nHost: [2001:0DB8::1]:080\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "absolute-form ipv6 port compare should use numeric value");
}

void test_parse_absolute_form_host_mismatch() {
    const std::string raw = "GET http://example.com/healthz HTTP/1.1\r\nHost: other.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "absolute-form host mismatch should fail");
}

void test_parse_absolute_form_ipvfuture_case_insensitive_match() {
    const std::string raw = "GET http://[v1.fe80::a]/healthz HTTP/1.1\r\nHost: [V1.FE80::A]\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete,
                       "absolute-form ipvfuture match should be case-insensitive");
}

void test_parse_absolute_form_userinfo_host_match() {
    const std::string raw = "GET http://user:pass@example.com/healthz HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "host should match authority without userinfo");
}

void test_parse_connect_authority_host_match() {
    const std::string raw = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    test::expect_equal(parsed.status, http::ParseStatus::Complete, "connect authority should match host");
}

void test_parse_connect_authority_host_mismatch() {
    const std::string raw = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:8443\r\n\r\n";
    const auto parsed = nebula::http::parse_http_request(raw, kMaxHeader, kMaxBody, kMaxRequestTarget);
    expect_parse_error(parsed, http::HttpStatus::BadRequest, "connect host mismatch should fail");
}

int run_parser_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"parse GET request", test_parse_get_request},
        {"parse GET request with query uses path only for routing",
         test_parse_get_request_with_query_uses_path_only_for_routing},
        {"parse GET request with query keeps kv shape after fragment",
         test_parse_get_request_with_query_keeps_kv_shape_after_fragment},
        {"parse GET request with duplicate query key preserves all values",
         test_parse_get_request_with_duplicate_query_key_preserves_all_values},
        {"parse path segments root", test_parse_path_segments_root},
        {"parse path segments trailing slash", test_parse_path_segments_trailing_slash},
        {"parse path segments repeated slash", test_parse_path_segments_repeated_slash},
        {"parse POST with body", test_parse_post_with_body},
        {"parse need more body", test_parse_need_more_body},
        {"parse bad request line", test_parse_bad_request_line},
        {"parse unsupported method", test_parse_unsupported_method},
        {"parse unsupported http version", test_parse_unsupported_http_version},
        {"parse invalid http version", test_parse_invalid_http_version},
        {"parse invalid header", test_parse_invalid_header},
        {"parse invalid header key char", test_parse_invalid_header_key_char},
        {"parse header key space before colon rejected", test_parse_header_key_space_before_colon_rejected},
        {"parse header key token chars allowed", test_parse_header_key_token_chars_allowed},
        {"parse header value with ctl rejected", test_parse_header_value_with_ctl_rejected},
        {"parse header value with del rejected", test_parse_header_value_with_del_rejected},
        {"parse invalid content-length", test_parse_invalid_content_length},
        {"parse content too large", test_parse_content_too_large},
        {"parse content-length sum overflow rejected", test_parse_content_length_sum_overflow_rejected},
        {"parse header too large", test_parse_header_too_large},
        {"parse uri too long", test_parse_uri_too_long},
        {"parse options asterisk-form", test_parse_options_asterisk_form},
        {"parse get asterisk-form rejected", test_parse_get_asterisk_form_rejected},
        {"parse post asterisk-form rejected", test_parse_post_asterisk_form_rejected},
        {"parse origin-form without leading slash rejected", test_parse_origin_form_without_leading_slash_rejected},
        {"parse connect requires authority-form", test_parse_connect_requires_authority_form},
        {"parse connection close", test_parse_connection_close},
        {"parse connection close in token list", test_parse_connection_close_in_token_list},
        {"parse http10 connection keep-alive in token list", test_parse_http10_connection_keep_alive_in_token_list},
        {"parse duplicate header rejected", test_parse_duplicate_header_rejected},
        {"parse duplicate content-length same value", test_parse_duplicate_content_length_same_value},
        {"parse duplicate content-length conflict", test_parse_duplicate_content_length_conflict},
        {"parse chunked body", test_parse_chunked_body},
        {"parse chunked body with extension and trailer", test_parse_chunked_body_with_extension_and_trailer},
        {"parse chunked need more", test_parse_chunked_need_more},
        {"parse chunked incremental context across fragments", test_parse_chunked_incremental_context_across_fragments},
        {"parse context resets after complete for pipeline", test_parse_context_resets_after_complete_for_pipeline},
        {"parse chunked context releases decoded capacity after complete",
         test_parse_chunked_context_releases_decoded_capacity_after_complete},
        {"parse context resets after error for reuse", test_parse_context_resets_after_error_for_reuse},
        {"parse chunked with content-length rejected", test_parse_chunked_with_content_length_rejected},
        {"parse transfer-encoding unsupported", test_parse_transfer_encoding_unsupported},
        {"parse invalid chunked body", test_parse_invalid_chunked_body},
        {"parse chunked size with leading whitespace rejected",
         test_parse_chunked_size_with_leading_whitespace_rejected},
        {"parse chunked size with whitespace before extension rejected",
         test_parse_chunked_size_with_whitespace_before_extension_rejected},
        {"parse chunked content too large", test_parse_chunked_content_too_large},
        {"parse chunked consumed bytes for pipeline", test_parse_chunked_consumed_bytes_for_pipeline},
        {"parse consumed bytes for pipeline", test_parse_consumed_bytes_for_pipeline},
        {"parse http11 missing host rejected", test_parse_http11_missing_host_rejected},
        {"parse http10 without host allowed", test_parse_http10_without_host_allowed},
        {"parse empty host header rejected", test_parse_empty_host_header_rejected},
        {"parse invalid host header rejected", test_parse_invalid_host_header_rejected},
        {"parse invalid host port rejected", test_parse_invalid_host_port_rejected},
        {"parse valid ipv4 host header", test_parse_valid_ipv4_host_header},
        {"parse invalid ipv4 host header rejected", test_parse_invalid_ipv4_host_header_rejected},
        {"parse ipv4 with leading zero rejected", test_parse_ipv4_with_leading_zero_rejected},
        {"parse valid ipv6 host header", test_parse_valid_ipv6_host_header},
        {"parse invalid ipv6 host header rejected", test_parse_invalid_ipv6_host_header_rejected},
        {"parse out of range host port rejected", test_parse_out_of_range_host_port_rejected},
        {"parse valid ipvfuture host header", test_parse_valid_ipvfuture_host_header},
        {"parse invalid ipvfuture host header rejected", test_parse_invalid_ipvfuture_host_header_rejected},
        {"parse absolute form host match", test_parse_absolute_form_host_match},
        {"parse absolute form without path normalized to root",
         test_parse_absolute_form_without_path_normalized_to_root},
        {"parse absolute form keeps original request line", test_parse_absolute_form_keeps_original_request_line},
        {"parse absolute form query only normalized with root",
         test_parse_absolute_form_query_only_normalized_with_root},
        {"parse absolute form host case insensitive match", test_parse_absolute_form_host_case_insensitive_match},
        {"parse absolute form ipv6 compressed equivalent match",
         test_parse_absolute_form_ipv6_compressed_equivalent_match},
        {"parse absolute form ipv6 port numeric equivalent match",
         test_parse_absolute_form_ipv6_port_numeric_equivalent_match},
        {"parse absolute form host mismatch", test_parse_absolute_form_host_mismatch},
        {"parse absolute form ipvfuture case insensitive match",
         test_parse_absolute_form_ipvfuture_case_insensitive_match},
        {"parse absolute form userinfo host match", test_parse_absolute_form_userinfo_host_match},
        {"parse connect authority host match", test_parse_connect_authority_host_match},
        {"parse connect authority host mismatch", test_parse_connect_authority_host_mismatch},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_parser_tests);
}
