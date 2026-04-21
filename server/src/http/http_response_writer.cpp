#include "nebula/http/http_response_writer.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string_view>
#include <utility>

namespace nebula::http {

namespace {

bool iequals_ascii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (std::size_t idx = 0; idx < lhs.size(); ++idx) {
        const auto left = static_cast<unsigned char>(lhs[idx]);
        const auto right = static_cast<unsigned char>(rhs[idx]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }

    return true;
}

bool is_tchar(unsigned char ch) {
    static constexpr std::string_view kTcharPunct = "!#$%&'*+-.^_`|~";
    return std::isalnum(ch) != 0 || kTcharPunct.find(static_cast<char>(ch)) != std::string_view::npos;
}

bool is_valid_header_name(std::string_view key) {
    if (key.empty()) {
        return false;
    }

    return std::ranges::all_of(key, [](unsigned char ch) { return is_tchar(ch); });
}

bool is_valid_header_value(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char ch) {
        if (ch == '\r' || ch == '\n') {
            return false;
        }
        return (ch >= 0x20U || ch == '\t') && ch != 0x7FU;
    });
}

bool is_managed_response_header(std::string_view key) {
    return iequals_ascii(key, "Content-Length") || iequals_ascii(key, "Connection");
}

bool status_disallows_body(HttpStatus status) {
    const int code = to_status_code(status);
    return (code >= 100 && code < 200) || code == 204 || code == 304;
}

}  // namespace

HttpResponse make_plain_text_response(HttpStatus status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.headers.emplace("Content-Type", "text/plain; charset=utf-8");
    response.body = std::move(body);
    return response;
}

HttpResponse make_json_response(HttpStatus status, const common::JsonValue& body) {
    HttpResponse response;
    response.status = status;
    response.headers.emplace("Content-Type", "application/json; charset=utf-8");
    response.body = common::dump_json(body);
    return response;
}

HttpResponse make_redirect_response(HttpStatus status, std::string location) {
    HttpResponse response;
    response.status = status;
    response.headers.emplace("Location", std::move(location));
    return response;
}

HttpResponse make_api_success_response(common::JsonValue data) {
    common::JsonObject body;
    body.emplace("code", common::JsonValue("ok"));
    body.emplace("message", common::JsonValue("success"));
    body.emplace("data", std::move(data));
    return make_json_response(HttpStatus::OK, common::JsonValue(std::move(body)));
}

HttpResponse make_api_error_response(HttpStatus status) {
    const HttpErrorInfo error_info = to_error_info(status);
    return make_api_error_response(error_info.status, error_info.code, error_info.message);
}

HttpResponse make_api_error_response(HttpStatus status, std::string_view message) {
    const HttpErrorInfo error_info = to_error_info(status);
    return make_api_error_response(error_info.status, error_info.code, message.empty() ? error_info.message : message);
}

HttpResponse make_api_error_response(HttpStatus status, std::string_view code, std::string_view message) {
    common::JsonObject body;
    body.emplace("code", common::JsonValue(code));
    body.emplace("message", common::JsonValue(message));
    body.emplace("data", common::JsonValue(nullptr));
    return make_json_response(status, common::JsonValue(std::move(body)));
}

std::string serialize_http_response(const HttpResponse& response, bool keep_alive, bool suppress_body) {
    const bool suppress_body_for_status = status_disallows_body(response.status);
    const bool omit_body = suppress_body || suppress_body_for_status;
    const std::size_t content_length = suppress_body_for_status ? 0U : response.body.size();

    HeaderMap headers;
    headers.reserve(response.headers.size() + 2U);
    for (const auto& [key, value] : response.headers) {
        if (is_managed_response_header(key)) {
            continue;
        }
        if (!is_valid_header_name(key) || !is_valid_header_value(value)) {
            continue;
        }
        headers.emplace(key, value);
    }
    headers["Content-Length"] = std::to_string(content_length);
    headers["Connection"] = keep_alive ? "keep-alive" : "close";

    std::string out = std::format("HTTP/1.1 {} {}\r\n", response.status_code(), response.status_text());
    for (const auto& [key, value] : headers) {
        out.append(key);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    out.append("\r\n");
    if (!omit_body) {
        out.append(response.body);
    }
    return out;
}

}  // namespace nebula::http
