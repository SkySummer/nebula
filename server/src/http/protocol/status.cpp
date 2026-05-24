#include "nebula/http/protocol/status.hpp"

#include <utility>

namespace nebula::http {

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
    std::unreachable();
}

}  // namespace nebula::http
