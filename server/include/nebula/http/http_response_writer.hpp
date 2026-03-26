#ifndef NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP
#define NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP

#include <string>

#include "nebula/http/http_types.hpp"

namespace nebula::http {

HttpResponse make_plain_text_response(HttpStatus status, std::string body);
HttpResponse make_error_response(HttpStatus status, std::string body = {});

std::string serialize_http_response(const HttpResponse& response, bool keep_alive, bool suppress_body = false);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP
