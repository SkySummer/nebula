#ifndef NEBULA_HTTP_HTTP_TYPES_HPP
#define NEBULA_HTTP_HTTP_TYPES_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nebula::http {

enum class HttpMethod : std::uint8_t {
    Unknown,
    Get,
    Head,
    Post,
    Put,
    Delete,
    Connect,
    Options,
    Trace,
};

[[nodiscard]] std::string_view to_string(HttpMethod method);

HttpMethod parse_method(std::string_view text);

enum class HttpStatus : std::uint16_t {
    Continue = 100,
    SwitchingProtocols = 101,
    Processing = 102,
    EarlyHints = 103,
    OK = 200,
    Created = 201,
    Accepted = 202,
    NonAuthoritativeInformation = 203,
    NoContent = 204,
    ResetContent = 205,
    PartialContent = 206,
    MultiStatus = 207,
    AlreadyReported = 208,
    IMUsed = 226,
    MultipleChoices = 300,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    UseProxy = 305,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,
    BadRequest = 400,
    Unauthorized = 401,
    PaymentRequired = 402,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    ProxyAuthenticationRequired = 407,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PreconditionFailed = 412,
    ContentTooLarge = 413,
    URITooLong = 414,
    UnsupportedMediaType = 415,
    RangeNotSatisfiable = 416,
    ExpectationFailed = 417,
    MisdirectedRequest = 421,
    UnprocessableContent = 422,
    Locked = 423,
    FailedDependency = 424,
    TooEarly = 425,
    UpgradeRequired = 426,
    PreconditionRequired = 428,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    UnavailableForLegalReasons = 451,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HTTPVersionNotSupported = 505,
    VariantAlsoNegotiates = 506,
    InsufficientStorage = 507,
    LoopDetected = 508,
    NetworkAuthenticationRequired = 511,
};

int to_status_code(HttpStatus status);

[[nodiscard]] std::string_view to_string(HttpStatus status);

using HeaderMap = std::unordered_map<std::string, std::string>;

struct HttpRequest {
    HttpMethod method = HttpMethod::Unknown;
    std::string path;
    std::string request_line;
    HeaderMap headers;
    std::string body;
    bool keep_alive = true;
};

struct HttpResponse {
    HttpStatus status = HttpStatus::OK;
    HeaderMap headers;
    std::string body;

    [[nodiscard]] int status_code() const {
        return to_status_code(status);
    }

    [[nodiscard]] std::string_view status_text() const {
        return to_string(status);
    }
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_HTTP_TYPES_HPP
