#ifndef NEBULA_AUTH_HTTP_AUTHENTICATION_HPP
#define NEBULA_AUTH_HTTP_AUTHENTICATION_HPP

#include <memory>

#include "nebula/auth/application/service.hpp"
#include "nebula/http/protocol/headers.hpp"

namespace nebula::auth {

[[nodiscard]] std::expected<AuthenticateResult, AuthError> authenticate_http_request(
    const std::shared_ptr<AuthService>& auth_service, const http::HeaderMap& headers);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_HTTP_AUTHENTICATION_HPP
