#ifndef NEBULA_AUTH_AUTH_HTTP_HPP
#define NEBULA_AUTH_AUTH_HTTP_HPP

#include <memory>

#include "nebula/server/server_config.hpp"

namespace nebula::http {

class Router;

}  // namespace nebula::http

namespace nebula::auth {

[[nodiscard]] bool register_auth_routes(const server::ServerConfig& config,
                                        const std::shared_ptr<http::Router>& router);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_AUTH_HTTP_HPP
