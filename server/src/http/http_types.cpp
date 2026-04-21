#include "nebula/http/http_types.hpp"

#include <algorithm>

namespace nebula::http {

namespace {

bool is_http_error_status(HttpStatus status) noexcept {
    const int code = to_status_code(status);
    return code >= 400;
}

}  // namespace

std::vector<std::string> split_http_path_segments(std::string_view path) {
    std::vector<std::string> segments;
    segments.reserve(static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) + 1U);

    std::size_t cursor = 0;
    while (true) {
        const std::size_t slash_pos = path.find('/', cursor);
        if (slash_pos == std::string_view::npos) {
            segments.emplace_back(path.substr(cursor));
            break;
        }

        segments.emplace_back(path.substr(cursor, slash_pos - cursor));
        cursor = slash_pos + 1U;
    }

    return segments;
}

std::string_view to_string(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Connect:
            return "CONNECT";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::Trace:
            return "TRACE";
        case HttpMethod::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

HttpMethod parse_method(std::string_view text) {
    if (text == "GET") {
        return HttpMethod::Get;
    }
    if (text == "HEAD") {
        return HttpMethod::Head;
    }
    if (text == "POST") {
        return HttpMethod::Post;
    }
    if (text == "PUT") {
        return HttpMethod::Put;
    }
    if (text == "DELETE") {
        return HttpMethod::Delete;
    }
    if (text == "CONNECT") {
        return HttpMethod::Connect;
    }
    if (text == "OPTIONS") {
        return HttpMethod::Options;
    }
    if (text == "TRACE") {
        return HttpMethod::Trace;
    }
    return HttpMethod::Unknown;
}

int to_status_code(HttpStatus status) noexcept {
    return static_cast<int>(status);
}

std::string_view to_string(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::Continue:
            return "Continue";
        case HttpStatus::SwitchingProtocols:
            return "Switching Protocols";
        case HttpStatus::Processing:
            return "Processing";
        case HttpStatus::EarlyHints:
            return "Early Hints";
        case HttpStatus::OK:
            return "OK";
        case HttpStatus::Created:
            return "Created";
        case HttpStatus::Accepted:
            return "Accepted";
        case HttpStatus::NonAuthoritativeInformation:
            return "Non-Authoritative Information";
        case HttpStatus::NoContent:
            return "No Content";
        case HttpStatus::ResetContent:
            return "Reset Content";
        case HttpStatus::PartialContent:
            return "Partial Content";
        case HttpStatus::MultiStatus:
            return "Multi-Status";
        case HttpStatus::AlreadyReported:
            return "Already Reported";
        case HttpStatus::IMUsed:
            return "IM Used";
        case HttpStatus::MultipleChoices:
            return "Multiple Choices";
        case HttpStatus::MovedPermanently:
            return "Moved Permanently";
        case HttpStatus::Found:
            return "Found";
        case HttpStatus::SeeOther:
            return "See Other";
        case HttpStatus::NotModified:
            return "Not Modified";
        case HttpStatus::UseProxy:
            return "Use Proxy";
        case HttpStatus::TemporaryRedirect:
            return "Temporary Redirect";
        case HttpStatus::PermanentRedirect:
            return "Permanent Redirect";
        case HttpStatus::BadRequest:
            return "Bad Request";
        case HttpStatus::Unauthorized:
            return "Unauthorized";
        case HttpStatus::PaymentRequired:
            return "Payment Required";
        case HttpStatus::Forbidden:
            return "Forbidden";
        case HttpStatus::NotFound:
            return "Not Found";
        case HttpStatus::MethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatus::NotAcceptable:
            return "Not Acceptable";
        case HttpStatus::ProxyAuthenticationRequired:
            return "Proxy Authentication Required";
        case HttpStatus::RequestTimeout:
            return "Request Timeout";
        case HttpStatus::Conflict:
            return "Conflict";
        case HttpStatus::Gone:
            return "Gone";
        case HttpStatus::LengthRequired:
            return "Length Required";
        case HttpStatus::PreconditionFailed:
            return "Precondition Failed";
        case HttpStatus::ContentTooLarge:
            return "Content Too Large";
        case HttpStatus::URITooLong:
            return "URI Too Long";
        case HttpStatus::UnsupportedMediaType:
            return "Unsupported Media Type";
        case HttpStatus::RangeNotSatisfiable:
            return "Range Not Satisfiable";
        case HttpStatus::ExpectationFailed:
            return "Expectation Failed";
        case HttpStatus::MisdirectedRequest:
            return "Misdirected Request";
        case HttpStatus::UnprocessableContent:
            return "Unprocessable Content";
        case HttpStatus::Locked:
            return "Locked";
        case HttpStatus::FailedDependency:
            return "Failed Dependency";
        case HttpStatus::TooEarly:
            return "Too Early";
        case HttpStatus::UpgradeRequired:
            return "Upgrade Required";
        case HttpStatus::PreconditionRequired:
            return "Precondition Required";
        case HttpStatus::TooManyRequests:
            return "Too Many Requests";
        case HttpStatus::RequestHeaderFieldsTooLarge:
            return "Request Header Fields Too Large";
        case HttpStatus::UnavailableForLegalReasons:
            return "Unavailable For Legal Reasons";
        case HttpStatus::InternalServerError:
            return "Internal Server Error";
        case HttpStatus::NotImplemented:
            return "Not Implemented";
        case HttpStatus::BadGateway:
            return "Bad Gateway";
        case HttpStatus::ServiceUnavailable:
            return "Service Unavailable";
        case HttpStatus::GatewayTimeout:
            return "Gateway Timeout";
        case HttpStatus::HTTPVersionNotSupported:
            return "HTTP Version Not Supported";
        case HttpStatus::VariantAlsoNegotiates:
            return "Variant Also Negotiates";
        case HttpStatus::InsufficientStorage:
            return "Insufficient Storage";
        case HttpStatus::LoopDetected:
            return "Loop Detected";
        case HttpStatus::NetworkAuthenticationRequired:
            return "Network Authentication Required";
    }
    return "Unknown";
}

HttpErrorInfo to_error_info(HttpStatus status) noexcept {
    if (!is_http_error_status(status)) {
        return to_error_info(HttpStatus::InternalServerError);
    }

    HttpErrorInfo error_info;
    error_info.status = status;

    switch (status) {
        case HttpStatus::BadRequest:
            error_info.code = "bad_request";
            error_info.message = "bad request";
            break;
        case HttpStatus::Unauthorized:
            error_info.code = "unauthorized";
            error_info.message = "unauthorized";
            break;
        case HttpStatus::PaymentRequired:
            error_info.code = "payment_required";
            error_info.message = "payment required";
            break;
        case HttpStatus::Forbidden:
            error_info.code = "forbidden";
            error_info.message = "forbidden";
            break;
        case HttpStatus::NotFound:
            error_info.code = "not_found";
            error_info.message = "not found";
            break;
        case HttpStatus::MethodNotAllowed:
            error_info.code = "method_not_allowed";
            error_info.message = "method not allowed";
            break;
        case HttpStatus::NotAcceptable:
            error_info.code = "not_acceptable";
            error_info.message = "not acceptable";
            break;
        case HttpStatus::ProxyAuthenticationRequired:
            error_info.code = "proxy_authentication_required";
            error_info.message = "proxy authentication required";
            break;
        case HttpStatus::RequestTimeout:
            error_info.code = "request_timeout";
            error_info.message = "request timeout";
            break;
        case HttpStatus::Conflict:
            error_info.code = "conflict";
            error_info.message = "conflict";
            break;
        case HttpStatus::Gone:
            error_info.code = "gone";
            error_info.message = "gone";
            break;
        case HttpStatus::LengthRequired:
            error_info.code = "length_required";
            error_info.message = "length required";
            break;
        case HttpStatus::PreconditionFailed:
            error_info.code = "precondition_failed";
            error_info.message = "precondition failed";
            break;
        case HttpStatus::ContentTooLarge:
            error_info.code = "content_too_large";
            error_info.message = "content too large";
            break;
        case HttpStatus::URITooLong:
            error_info.code = "uri_too_long";
            error_info.message = "uri too long";
            break;
        case HttpStatus::UnsupportedMediaType:
            error_info.code = "unsupported_media_type";
            error_info.message = "unsupported media type";
            break;
        case HttpStatus::RangeNotSatisfiable:
            error_info.code = "range_not_satisfiable";
            error_info.message = "range not satisfiable";
            break;
        case HttpStatus::ExpectationFailed:
            error_info.code = "expectation_failed";
            error_info.message = "expectation failed";
            break;
        case HttpStatus::MisdirectedRequest:
            error_info.code = "misdirected_request";
            error_info.message = "misdirected request";
            break;
        case HttpStatus::UnprocessableContent:
            error_info.code = "unprocessable_content";
            error_info.message = "unprocessable content";
            break;
        case HttpStatus::Locked:
            error_info.code = "locked";
            error_info.message = "locked";
            break;
        case HttpStatus::FailedDependency:
            error_info.code = "failed_dependency";
            error_info.message = "failed dependency";
            break;
        case HttpStatus::TooEarly:
            error_info.code = "too_early";
            error_info.message = "too early";
            break;
        case HttpStatus::UpgradeRequired:
            error_info.code = "upgrade_required";
            error_info.message = "upgrade required";
            break;
        case HttpStatus::PreconditionRequired:
            error_info.code = "precondition_required";
            error_info.message = "precondition required";
            break;
        case HttpStatus::TooManyRequests:
            error_info.code = "too_many_requests";
            error_info.message = "too many requests";
            break;
        case HttpStatus::RequestHeaderFieldsTooLarge:
            error_info.code = "request_header_fields_too_large";
            error_info.message = "request header fields too large";
            break;
        case HttpStatus::UnavailableForLegalReasons:
            error_info.code = "unavailable_for_legal_reasons";
            error_info.message = "unavailable for legal reasons";
            break;
        case HttpStatus::InternalServerError:
            error_info.code = "internal_server_error";
            error_info.message = "internal server error";
            break;
        case HttpStatus::NotImplemented:
            error_info.code = "not_implemented";
            error_info.message = "not implemented";
            break;
        case HttpStatus::BadGateway:
            error_info.code = "bad_gateway";
            error_info.message = "bad gateway";
            break;
        case HttpStatus::ServiceUnavailable:
            error_info.code = "service_unavailable";
            error_info.message = "service unavailable";
            break;
        case HttpStatus::GatewayTimeout:
            error_info.code = "gateway_timeout";
            error_info.message = "gateway timeout";
            break;
        case HttpStatus::HTTPVersionNotSupported:
            error_info.code = "http_version_not_supported";
            error_info.message = "http version not supported";
            break;
        case HttpStatus::VariantAlsoNegotiates:
            error_info.code = "variant_also_negotiates";
            error_info.message = "variant also negotiates";
            break;
        case HttpStatus::InsufficientStorage:
            error_info.code = "insufficient_storage";
            error_info.message = "insufficient storage";
            break;
        case HttpStatus::LoopDetected:
            error_info.code = "loop_detected";
            error_info.message = "loop detected";
            break;
        case HttpStatus::NetworkAuthenticationRequired:
            error_info.code = "network_authentication_required";
            error_info.message = "network authentication required";
            break;
        default:
            error_info.code = "unknown";
            error_info.message = "unknown";
            break;
    }
    return error_info;
}

}  // namespace nebula::http
