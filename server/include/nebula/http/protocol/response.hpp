#ifndef NEBULA_HTTP_PROTOCOL_RESPONSE_HPP
#define NEBULA_HTTP_PROTOCOL_RESPONSE_HPP

#include <string>
#include <string_view>

#include "nebula/http/protocol/headers.hpp"
#include "nebula/http/protocol/status.hpp"

namespace nebula::http {

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

#endif  // NEBULA_HTTP_PROTOCOL_RESPONSE_HPP
