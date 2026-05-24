#ifndef NEBULA_AUTH_HTTP_HANDLERS_HPP
#define NEBULA_AUTH_HTTP_HANDLERS_HPP

#include <memory>

#include "nebula/auth/application/service.hpp"
#include "nebula/http/routing/router.hpp"

namespace nebula::auth {

[[nodiscard]] http::HttpResponse handle_register(const std::shared_ptr<AuthService>& auth_service,
                                                 const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_login(const std::shared_ptr<AuthService>& auth_service,
                                              const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_me(const std::shared_ptr<AuthService>& auth_service,
                                           const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_change_password(const std::shared_ptr<AuthService>& auth_service,
                                                        const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_list_users(const std::shared_ptr<AuthService>& auth_service,
                                                   const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_get_user(const std::shared_ptr<AuthService>& auth_service,
                                                 const http::RouteContext& context);

[[nodiscard]] http::HttpResponse handle_update_user(const std::shared_ptr<AuthService>& auth_service,
                                                    const http::RouteContext& context);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_HTTP_HANDLERS_HPP
