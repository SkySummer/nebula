#ifndef NEBULA_AUTH_AUTH_HTTP_HPP
#define NEBULA_AUTH_AUTH_HTTP_HPP

#include <memory>

#include "nebula/auth/auth_service.hpp"
#include "nebula/http/http_types.hpp"
#include "nebula/http/router.hpp"

namespace nebula::auth {

[[nodiscard]] bool register_auth_routes(const std::shared_ptr<AuthService>& auth_service,
                                        const std::shared_ptr<http::Router>& router);

[[nodiscard]] http::HttpResponse make_auth_error_response(AuthErrorCode error);

[[nodiscard]] AuthenticateResult authenticate_http_request(const std::shared_ptr<AuthService>& auth_service,
                                                           const http::HeaderMap& headers);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_AUTH_HTTP_HPP
