#include "nebula/http/protocol/method.hpp"

#include <utility>

namespace nebula::http {

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
    std::unreachable();
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

}  // namespace nebula::http
