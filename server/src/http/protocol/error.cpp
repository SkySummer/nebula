#include "nebula/http/protocol/error.hpp"

#include <utility>

namespace nebula::http {

namespace {

bool is_http_error_status(HttpStatus status) noexcept {
    const int code = to_status_code(status);
    return code >= 400;
}

}  // namespace

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
        case HttpStatus::Continue:
        case HttpStatus::SwitchingProtocols:
        case HttpStatus::Processing:
        case HttpStatus::EarlyHints:
        case HttpStatus::OK:
        case HttpStatus::Created:
        case HttpStatus::Accepted:
        case HttpStatus::NonAuthoritativeInformation:
        case HttpStatus::NoContent:
        case HttpStatus::ResetContent:
        case HttpStatus::PartialContent:
        case HttpStatus::MultiStatus:
        case HttpStatus::AlreadyReported:
        case HttpStatus::IMUsed:
        case HttpStatus::MultipleChoices:
        case HttpStatus::MovedPermanently:
        case HttpStatus::Found:
        case HttpStatus::SeeOther:
        case HttpStatus::NotModified:
        case HttpStatus::UseProxy:
        case HttpStatus::TemporaryRedirect:
        case HttpStatus::PermanentRedirect:
            std::unreachable();
    }

    return error_info;
}

}  // namespace nebula::http
