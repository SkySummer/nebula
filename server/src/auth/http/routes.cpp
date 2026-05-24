#include "nebula/auth/http/routes.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/auth/http/handlers.hpp"
#include "nebula/common/log/logger.hpp"

namespace nebula::auth {

namespace {

bool register_route(const std::shared_ptr<http::Router>& router, http::HttpMethod method, std::string_view path,
                    http::RouteHandler handler, http::RouteOptions options = {}) {
    const bool added = router->add_route(method, std::string(path), std::move(handler), options);
    if (added) {
        return true;
    }

    common::Logger::instance()
        .fatal("register auth route failed")
        .field("method", http::to_string(method))
        .field("path", path)
        .field("error", "register_route_failed")
        .field("decision", "exit_process");
    return false;
}

}  // namespace

bool register_auth_routes(const std::shared_ptr<http::Router>& router, const std::shared_ptr<AuthService>& service) {
    if (router == nullptr || service == nullptr) {
        return false;
    }

    if (!register_route(router, http::HttpMethod::Post, "/api/auth/register",
                        std::bind_front(handle_register, service))) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/auth/login", std::bind_front(handle_login, service))) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/auth/me", std::bind_front(handle_me, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/auth/password",
                        std::bind_front(handle_change_password, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/auth/users", std::bind_front(handle_list_users, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/auth/users/{user_id}",
                        std::bind_front(handle_get_user, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/auth/users/{user_id}",
                        std::bind_front(handle_update_user, service), http::RouteOptions{.require_user = true})) {
        return false;
    }

    return true;
}

}  // namespace nebula::auth
