#ifndef NEBULA_AUTH_HTTP_RESPONSES_HPP
#define NEBULA_AUTH_HTTP_RESPONSES_HPP

#include "nebula/auth/domain/error.hpp"
#include "nebula/http/protocol/response.hpp"

namespace nebula::auth {

[[nodiscard]] http::HttpResponse to_http_response(AuthError error);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_HTTP_RESPONSES_HPP
