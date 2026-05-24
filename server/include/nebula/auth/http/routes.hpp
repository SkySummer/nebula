#ifndef NEBULA_AUTH_HTTP_ROUTES_HPP
#define NEBULA_AUTH_HTTP_ROUTES_HPP

#include <memory>

#include "nebula/http/routing/router.hpp"

namespace nebula::auth {

class AuthService;

[[nodiscard]] bool register_auth_routes(const std::shared_ptr<http::Router>& router,
                                        const std::shared_ptr<AuthService>& service);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_HTTP_ROUTES_HPP
