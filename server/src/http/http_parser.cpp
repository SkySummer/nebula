#include "nebula/http/http_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

#include <arpa/inet.h>

#include "nebula/http/http_types.hpp"

namespace nebula::http {

namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string_view trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(begin, end - begin);
}

bool is_all_digits(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    return std::ranges::all_of(text, [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool equals_ignore_case_ascii(std::string_view lhs, std::string_view rhs) {
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

bool is_valid_token(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    return std::ranges::all_of(text, [](unsigned char ch) { return is_tchar(ch); });
}

bool is_valid_header_value(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char ch) { return (ch >= 0x20U || ch == '\t') && ch != 0x7FU; });
}

bool is_hex_digit(unsigned char ch) {
    return std::isxdigit(ch) != 0;
}

bool is_valid_scheme(std::string_view scheme) {
    if (scheme.empty()) {
        return false;
    }

    if (std::isalpha(static_cast<unsigned char>(scheme.front())) == 0) {
        return false;
    }

    for (std::size_t idx = 1; idx < scheme.size(); ++idx) {
        const auto ch = static_cast<unsigned char>(scheme[idx]);
        if (std::isalnum(ch) != 0 || ch == '+' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

bool is_sub_delim(unsigned char ch) {
    static constexpr std::string_view kSubDelims = "!$&'()*+,;=";
    return kSubDelims.find(static_cast<char>(ch)) != std::string_view::npos;
}

bool is_unreserved(unsigned char ch) {
    if (std::isalnum(ch) != 0) {
        return true;
    }
    return ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

bool is_valid_reg_name(std::string_view host) {
    for (std::size_t idx = 0; idx < host.size(); ++idx) {
        const auto ch = static_cast<unsigned char>(host[idx]);
        if (is_unreserved(ch) || is_sub_delim(ch)) {
            continue;
        }

        if (ch == '%' && (idx + 2U) < host.size() && is_hex_digit(static_cast<unsigned char>(host[idx + 1U])) &&
            is_hex_digit(static_cast<unsigned char>(host[idx + 2U]))) {
            idx += 2U;
            continue;
        }

        return false;
    }
    return true;
}

bool contains_invalid_host_chars(std::string_view text) {
    return std::ranges::any_of(text, [](unsigned char ch) { return std::iscntrl(ch) != 0 || std::isspace(ch) != 0; });
}

bool looks_like_ipv4_literal(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    bool has_dot = false;
    for (const unsigned char ch : text) {
        if (ch == '.') {
            has_dot = true;
            continue;
        }
        if (std::isdigit(ch) == 0) {
            return false;
        }
    }
    return has_dot;
}

std::optional<std::array<unsigned char, 4>> parse_ipv4_address(std::string_view text) {
    std::array<unsigned char, 4> bytes{};
    std::size_t cursor = 0;
    for (std::size_t segment = 0; segment < bytes.size(); ++segment) {
        if (cursor >= text.size()) {
            return std::nullopt;
        }

        const std::size_t dot_pos = text.find('.', cursor);
        if (segment + 1U < bytes.size() && dot_pos == std::string_view::npos) {
            return std::nullopt;
        }
        if (segment + 1U == bytes.size() && dot_pos != std::string_view::npos) {
            return std::nullopt;
        }

        const std::string_view part =
            segment + 1U < bytes.size() ? text.substr(cursor, dot_pos - cursor) : text.substr(cursor);
        if (!is_all_digits(part)) {
            return std::nullopt;
        }
        if (part.size() > 1U && part.front() == '0') {
            return std::nullopt;
        }

        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc() || ptr != part.data() + part.size() || value > 255U) {
            return std::nullopt;
        }

        bytes.at(segment) = static_cast<unsigned char>(value);
        if (segment + 1U < bytes.size()) {
            cursor = dot_pos + 1U;
        } else {
            cursor = text.size();
        }
    }

    if (cursor != text.size()) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::array<unsigned char, 16>> parse_ipv6_address(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::array<unsigned char, 16> bytes{};
    const std::string ipv6_text(text);
    if (inet_pton(AF_INET6, ipv6_text.c_str(), bytes.data()) != 1) {
        return std::nullopt;
    }
    return bytes;
}

bool is_valid_ipvfuture(std::string_view literal) {
    if (literal.size() < 3U) {
        return false;
    }

    if (std::tolower(static_cast<unsigned char>(literal.front())) != 'v') {
        return false;
    }

    const std::size_t dot = literal.find('.');
    if (dot == std::string_view::npos || dot <= 1U || dot + 1U >= literal.size()) {
        return false;
    }

    const std::string_view version = literal.substr(1U, dot - 1U);
    if (!std::ranges::all_of(version, [](unsigned char ch) { return is_hex_digit(ch); })) {
        return false;
    }

    const std::string_view tail = literal.substr(dot + 1U);
    return std::ranges::all_of(tail,
                               [](unsigned char ch) { return is_unreserved(ch) || is_sub_delim(ch) || ch == ':'; });
}

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (!is_all_digits(text)) {
        return std::nullopt;
    }

    std::uint32_t port = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc() || ptr != end || port > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

struct HostCompareKey {
    using Ipv4Bytes = std::array<unsigned char, 4>;
    using Ipv6Bytes = std::array<unsigned char, 16>;
    using RegName = std::string;
    struct IpvFutureLiteral {
        std::string value;

        bool operator==(const IpvFutureLiteral& rhs) const = default;
    };

    std::variant<RegName, Ipv4Bytes, Ipv6Bytes, IpvFutureLiteral> address;
    std::optional<std::uint16_t> port;
};

std::optional<HostCompareKey> to_host_compare_key(std::string_view value) {
    HostCompareKey key;
    if (value.empty()) {
        return std::nullopt;
    }

    if (value.front() == '[') {
        const std::size_t close_bracket = value.find(']');
        if (close_bracket == std::string_view::npos || close_bracket <= 1U) {
            return std::nullopt;
        }

        const std::string_view literal = value.substr(1U, close_bracket - 1U);
        const auto ipv6 = parse_ipv6_address(literal);
        if (ipv6.has_value()) {
            key.address = ipv6.value();
        } else if (is_valid_ipvfuture(literal)) {
            key.address = HostCompareKey::IpvFutureLiteral{to_lower(literal)};
        } else {
            return std::nullopt;
        }

        if (close_bracket + 1U == value.size()) {
            return key;
        }
        if (value[close_bracket + 1U] != ':') {
            return std::nullopt;
        }
        key.port = parse_port(value.substr(close_bracket + 2U));
        if (!key.port.has_value()) {
            return std::nullopt;
        }
        return key;
    }

    const std::size_t first_colon = value.find(':');
    const std::size_t last_colon = value.rfind(':');
    std::string_view host_part = value;
    if (first_colon != std::string_view::npos) {
        if (first_colon != last_colon) {
            return std::nullopt;
        }
        host_part = value.substr(0, first_colon);
        key.port = parse_port(value.substr(first_colon + 1U));
        if (!key.port.has_value()) {
            return std::nullopt;
        }
    }

    if (host_part.empty()) {
        return std::nullopt;
    }
    if (looks_like_ipv4_literal(host_part)) {
        const auto ipv4 = parse_ipv4_address(host_part);
        if (!ipv4.has_value()) {
            return std::nullopt;
        }
        key.address = ipv4.value();
        return key;
    }
    if (!is_valid_reg_name(host_part)) {
        return std::nullopt;
    }
    key.address = to_lower(host_part);
    return key;
}

bool host_values_equivalent(std::string_view lhs, std::string_view rhs) {
    const auto lhs_key = to_host_compare_key(lhs);
    const auto rhs_key = to_host_compare_key(rhs);
    if (!lhs_key.has_value() || !rhs_key.has_value()) {
        return false;
    }

    if (lhs_key->port != rhs_key->port) {
        return false;
    }
    return lhs_key->address == rhs_key->address;
}

bool is_valid_uri_host(std::string_view value) {
    return to_host_compare_key(value).has_value();
}

bool is_valid_host_field_value(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.find('@') != std::string_view::npos) {
        return false;
    }
    if (contains_invalid_host_chars(value)) {
        return false;
    }
    return is_valid_uri_host(value);
}

std::string_view strip_authority_userinfo(std::string_view authority) {
    const std::size_t at_sign = authority.rfind('@');
    if (at_sign == std::string_view::npos) {
        return authority;
    }
    return authority.substr(at_sign + 1U);
}

std::optional<std::string_view> request_target_authority(HttpMethod method, std::string_view request_target) {
    if (method == HttpMethod::Connect) {
        return request_target;
    }
    if (request_target == "*") {
        return std::nullopt;
    }

    const std::size_t scheme_sep = request_target.find("://");
    if (scheme_sep == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view scheme = request_target.substr(0, scheme_sep);
    if (!is_valid_scheme(scheme)) {
        return std::nullopt;
    }

    const std::size_t authority_begin = scheme_sep + 3U;
    const std::size_t authority_end = request_target.find_first_of("/?#", authority_begin);
    if (authority_end == std::string_view::npos) {
        return request_target.substr(authority_begin);
    }
    return request_target.substr(authority_begin, authority_end - authority_begin);
}

std::string strip_query_and_fragment(std::string_view path_text) {
    const std::size_t path_end = path_text.find_first_of("?#");
    const std::string_view route_path = path_text.substr(0U, path_end);
    if (route_path.empty()) {
        return "/";
    }
    return std::string(route_path);
}

std::string normalize_request_path(HttpMethod method, std::string_view request_target) {
    if (method == HttpMethod::Connect || request_target == "*") {
        return std::string(request_target);
    }

    const std::size_t scheme_sep = request_target.find("://");
    if (scheme_sep == std::string_view::npos) {
        return strip_query_and_fragment(request_target);
    }

    const std::string_view scheme = request_target.substr(0, scheme_sep);
    if (!is_valid_scheme(scheme)) {
        return strip_query_and_fragment(request_target);
    }

    const std::size_t authority_begin = scheme_sep + 3U;
    const std::size_t suffix_begin = request_target.find_first_of("/?#", authority_begin);
    if (suffix_begin == std::string_view::npos) {
        return "/";
    }

    if (request_target[suffix_begin] == '/') {
        return strip_query_and_fragment(request_target.substr(suffix_begin));
    }
    return "/";
}

bool is_valid_request_target_form(HttpMethod method, std::string_view request_target) {
    if (request_target == "*") {
        return method == HttpMethod::Options;
    }

    if (method == HttpMethod::Connect) {
        const auto authority = to_host_compare_key(request_target);
        return authority.has_value() && authority->port.has_value();
    }

    if (request_target.starts_with('/')) {
        return true;
    }

    const std::optional<std::string_view> authority = request_target_authority(HttpMethod::Get, request_target);
    if (!authority.has_value()) {
        return false;
    }
    return is_valid_host_field_value(strip_authority_userinfo(authority.value()));
}

enum class VersionCheck : std::uint8_t {
    Supported,
    Unsupported,
    Invalid,
};

VersionCheck check_http_version(std::string_view version_text) {
    if (!version_text.starts_with("HTTP/")) {
        return VersionCheck::Invalid;
    }

    const std::string_view version_digits = version_text.substr(5U);
    if (version_digits.empty()) {
        return VersionCheck::Invalid;
    }

    const std::size_t dot_pos = version_digits.find('.');
    if (dot_pos == std::string_view::npos) {
        if (!is_all_digits(version_digits)) {
            return VersionCheck::Invalid;
        }
        return VersionCheck::Unsupported;
    }

    const std::string_view major = version_digits.substr(0U, dot_pos);
    const std::string_view minor = version_digits.substr(dot_pos + 1U);
    if (!is_all_digits(major) || !is_all_digits(minor)) {
        return VersionCheck::Invalid;
    }

    if (major == "1" && (minor == "0" || minor == "1")) {
        return VersionCheck::Supported;
    }
    return VersionCheck::Unsupported;
}

std::optional<std::size_t> parse_content_length(std::string_view text) {
    std::string_view trimmed = trim_ascii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::size_t length = 0;
    const char* begin = trimmed.data();
    const char* end = trimmed.data() + trimmed.size();
    const auto [ptr, ec] = std::from_chars(begin, end, length);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return length;
}

ParseResult parse_error(HttpStatus status, std::string_view message = {}) {
    ParseResult result;
    result.status = ParseStatus::Error;
    result.http_status = status;
    result.error = std::string(message);
    return result;
}

ParseResult parse_complete() {
    ParseResult result;
    result.status = ParseStatus::Complete;
    return result;
}

bool is_repeatable_header(std::string_view key) {
    static constexpr std::array<std::string_view, 1> kRepeatableHeaders = {"content-length"};
    return std::ranges::find(kRepeatableHeaders, key) != kRepeatableHeaders.end();
}

std::size_t request_line_content_end(std::string_view header_block) {
    const std::size_t request_line_end = header_block.find("\r\n");
    return request_line_end == std::string_view::npos ? header_block.size() : request_line_end;
}

ParseResult parse_request_line(std::string_view header_block, HttpRequest& request, std::string_view& version_text,
                               std::string_view& request_target, std::size_t& request_line_end,
                               std::size_t max_request_target_bytes) {
    request_line_end = header_block.find("\r\n");
    const std::string_view request_line = header_block.substr(0, request_line_content_end(header_block));
    request.request_line = std::string(request_line);

    const std::size_t method_end = request_line.find(' ');
    if (method_end == std::string_view::npos) {
        return parse_error(HttpStatus::BadRequest, "Invalid Request Line");
    }

    const std::size_t path_end = request_line.find(' ', method_end + 1U);
    if (path_end == std::string_view::npos) {
        return parse_error(HttpStatus::BadRequest, "Invalid Request Line");
    }

    const std::string_view method_text = request_line.substr(0, method_end);
    const std::string_view path_text = request_line.substr(method_end + 1U, path_end - method_end - 1U);
    version_text = request_line.substr(path_end + 1U);
    if (method_text.empty() || path_text.empty() || version_text.empty()) {
        return parse_error(HttpStatus::BadRequest, "Invalid Request Line");
    }
    if (path_text.size() > max_request_target_bytes) {
        return parse_error(HttpStatus::URITooLong);
    }

    const VersionCheck version_check = check_http_version(version_text);
    if (version_check == VersionCheck::Invalid) {
        return parse_error(HttpStatus::BadRequest, "Invalid HTTP Version");
    }
    if (version_check == VersionCheck::Unsupported) {
        return parse_error(HttpStatus::HTTPVersionNotSupported);
    }

    request.method = parse_method(method_text);
    if (request.method == HttpMethod::Unknown) {
        return parse_error(HttpStatus::NotImplemented, "Unsupported HTTP Method");
    }

    if (!is_valid_request_target_form(request.method, path_text)) {
        return parse_error(HttpStatus::BadRequest, "Invalid Request Target Form");
    }

    request_target = path_text;
    request.path = normalize_request_path(request.method, request_target);
    return parse_complete();
}

ParseResult merge_header(HttpRequest& request, const std::string& key, std::string_view raw_value) {
    const auto header_it = request.headers.find(key);
    if (header_it == request.headers.end()) {
        request.headers.emplace(key, std::string(raw_value));
        return parse_complete();
    }

    if (!is_repeatable_header(key)) {
        return parse_error(HttpStatus::BadRequest, "Duplicate Header");
    }

    if (key != "content-length") {
        return parse_complete();
    }

    const std::optional<std::size_t> existing_length = parse_content_length(header_it->second);
    const std::optional<std::size_t> incoming_length = parse_content_length(raw_value);
    if (!existing_length.has_value() || !incoming_length.has_value()) {
        return parse_error(HttpStatus::BadRequest, "Invalid Content-Length");
    }
    if (existing_length.value() != incoming_length.value()) {
        return parse_error(HttpStatus::BadRequest, "Conflicting Content-Length");
    }

    header_it->second = std::to_string(existing_length.value());
    return parse_complete();
}

ParseResult parse_headers(std::string_view header_block, std::size_t request_line_end, HttpRequest& request) {
    std::size_t cursor = request_line_end == std::string_view::npos ? header_block.size() : request_line_end + 2U;
    while (cursor < header_block.size()) {
        const std::size_t next_line = header_block.find("\r\n", cursor);
        const std::string_view line = next_line == std::string_view::npos
                                          ? header_block.substr(cursor)
                                          : header_block.substr(cursor, next_line - cursor);

        if (line.empty()) {
            return parse_error(HttpStatus::BadRequest, "Unexpected Empty Header Line");
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return parse_error(HttpStatus::BadRequest, "Invalid Header Line");
        }

        const std::string_view raw_key = line.substr(0, colon);
        const std::string_view raw_value = trim_ascii(line.substr(colon + 1U));
        if (!is_valid_token(raw_key)) {
            return parse_error(HttpStatus::BadRequest, "Invalid Header Key");
        }
        if (!is_valid_header_value(raw_value)) {
            return parse_error(HttpStatus::BadRequest, "Invalid Header Value");
        }

        const ParseResult merged = merge_header(request, to_lower(raw_key), raw_value);
        if (merged.status != ParseStatus::Complete) {
            return merged;
        }

        if (next_line == std::string_view::npos) {
            break;
        }
        cursor = next_line + 2U;
    }

    return parse_complete();
}

ParseResult resolve_content_length(const HttpRequest& request, std::size_t max_body_bytes,
                                   std::size_t& content_length) {
    if (request.headers.contains("transfer-encoding")) {
        return parse_error(HttpStatus::NotImplemented, "Unsupported Transfer-Encoding");
    }

    content_length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        const std::optional<std::size_t> parsed = parse_content_length(it->second);
        if (!parsed.has_value()) {
            return parse_error(HttpStatus::BadRequest, "Invalid Content-Length");
        }
        content_length = parsed.value();
    }

    if (content_length > max_body_bytes) {
        return parse_error(HttpStatus::ContentTooLarge);
    }

    return parse_complete();
}

ParseResult validate_host_header(const HttpRequest& request, std::string_view version_text,
                                 std::string_view request_target) {
    if (version_text != "HTTP/1.1") {
        return parse_complete();
    }

    const auto host_it = request.headers.find("host");
    if (host_it == request.headers.end()) {
        return parse_error(HttpStatus::BadRequest, "Missing Host Header");
    }

    const std::string_view host_value = trim_ascii(host_it->second);
    if (!is_valid_host_field_value(host_value)) {
        return parse_error(HttpStatus::BadRequest, "Invalid Host Header");
    }

    const std::optional<std::string_view> authority = request_target_authority(request.method, request_target);
    if (!authority.has_value()) {
        return parse_complete();
    }
    const std::string_view expected_host = strip_authority_userinfo(authority.value());
    if (!is_valid_host_field_value(expected_host)) {
        return parse_error(HttpStatus::BadRequest, "Invalid Request Target Authority");
    }
    if (!host_values_equivalent(host_value, expected_host)) {
        return parse_error(HttpStatus::BadRequest, "Host Header Mismatch");
    }

    return parse_complete();
}

bool has_connection_option(std::string_view value, std::string_view option) {
    std::size_t cursor = 0;
    while (cursor <= value.size()) {
        const std::size_t comma = value.find(',', cursor);
        const std::string_view part =
            comma == std::string_view::npos ? value.substr(cursor) : value.substr(cursor, comma - cursor);
        const std::string_view token = trim_ascii(part);
        if (!token.empty() && equals_ignore_case_ascii(token, option)) {
            return true;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1U;
    }
    return false;
}

void apply_connection_policy(HttpRequest& request, std::string_view version_text) {
    const bool http11 = version_text == "HTTP/1.1";
    request.keep_alive = http11;
    if (const auto connection_it = request.headers.find("connection"); connection_it != request.headers.end()) {
        const std::string_view connection_value = connection_it->second;
        const bool has_close = has_connection_option(connection_value, "close");
        const bool has_keep_alive = has_connection_option(connection_value, "keep-alive");
        if (has_close) {
            request.keep_alive = false;
        } else if (has_keep_alive) {
            request.keep_alive = true;
        }
    }
}

}  // namespace

ParseResult parse_http_request(std::string_view buffer, std::size_t max_header_bytes, std::size_t max_body_bytes,
                               std::size_t max_request_target_bytes) {
    const std::size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        if (buffer.size() > max_header_bytes) {
            return parse_error(HttpStatus::RequestHeaderFieldsTooLarge);
        }
        return ParseResult{};
    }

    const std::size_t header_bytes = header_end + 4U;
    if (header_bytes > max_header_bytes) {
        return parse_error(HttpStatus::RequestHeaderFieldsTooLarge);
    }

    HttpRequest request;
    const std::string_view header_block = buffer.substr(0, header_end);

    std::string_view version_text;
    std::string_view request_target;
    std::size_t request_line_end = std::string_view::npos;
    ParseResult parsed_line = parse_request_line(header_block, request, version_text, request_target, request_line_end,
                                                 max_request_target_bytes);
    if (parsed_line.status != ParseStatus::Complete) {
        return parsed_line;
    }

    ParseResult parsed_headers = parse_headers(header_block, request_line_end, request);
    if (parsed_headers.status != ParseStatus::Complete) {
        return parsed_headers;
    }

    ParseResult validated_host = validate_host_header(request, version_text, request_target);
    if (validated_host.status != ParseStatus::Complete) {
        return validated_host;
    }

    std::size_t content_length = 0;
    ParseResult resolved_length = resolve_content_length(request, max_body_bytes, content_length);
    if (resolved_length.status != ParseStatus::Complete) {
        return resolved_length;
    }

    if (content_length > (std::numeric_limits<std::size_t>::max() - header_bytes)) {
        return parse_error(HttpStatus::ContentTooLarge);
    }

    const std::size_t total_needed = header_bytes + content_length;
    if (buffer.size() < total_needed) {
        return ParseResult{};
    }

    if (content_length > 0U) {
        request.body = std::string(buffer.substr(header_bytes, content_length));
    }

    apply_connection_policy(request, version_text);

    ParseResult result;
    result.status = ParseStatus::Complete;
    result.consumed_bytes = total_needed;
    result.request = std::move(request);
    return result;
}

}  // namespace nebula::http
