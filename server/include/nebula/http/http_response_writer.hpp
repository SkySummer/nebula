#ifndef NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP
#define NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP

#include <string>
#include <string_view>

#include "nebula/common/json.hpp"
#include "nebula/http/http_types.hpp"

namespace nebula::http {

HttpResponse make_plain_text_response(HttpStatus status, std::string body);
HttpResponse make_json_response(HttpStatus status, const common::JsonValue& body);
HttpResponse make_redirect_response(HttpStatus status, std::string location);
HttpResponse make_api_success_response(common::JsonValue data);
HttpResponse make_api_error_response(HttpStatus status);
HttpResponse make_api_error_response(HttpStatus status, std::string_view message);
HttpResponse make_api_error_response(HttpStatus status, std::string_view code, std::string_view message);

std::string serialize_http_response(const HttpResponse& response, bool keep_alive, bool suppress_body = false);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_HTTP_RESPONSE_WRITER_HPP
