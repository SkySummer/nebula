#ifndef NEBULA_HTTP_PROTOCOL_ERROR_HPP
#define NEBULA_HTTP_PROTOCOL_ERROR_HPP

#include <string_view>

#include "nebula/http/protocol/status.hpp"

namespace nebula::http {

struct HttpErrorInfo {
    HttpStatus status = HttpStatus::InternalServerError;
    std::string_view code;
    std::string_view message;
};

[[nodiscard]] HttpErrorInfo to_error_info(HttpStatus status) noexcept;

}  // namespace nebula::http

#endif  // NEBULA_HTTP_PROTOCOL_ERROR_HPP
