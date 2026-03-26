#ifndef NEBULA_HTTP_HTTP_PARSER_HPP
#define NEBULA_HTTP_HTTP_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "nebula/http/http_types.hpp"

namespace nebula::http {

enum class ParseStatus : std::uint8_t {
    NeedMore,
    Complete,
    Error,
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    HttpStatus http_status = HttpStatus::OK;
    std::size_t consumed_bytes = 0;
    HttpRequest request;
    std::string error;
};

ParseResult parse_http_request(std::string_view buffer, std::size_t max_header_bytes, std::size_t max_body_bytes,
                               std::size_t max_request_target_bytes = std::numeric_limits<std::size_t>::max());

}  // namespace nebula::http

#endif  // NEBULA_HTTP_HTTP_PARSER_HPP
