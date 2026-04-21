#include "nebula/server/http_server.hpp"

#include <stdexcept>
#include <utility>

namespace nebula::server {

HttpServerBuilder& HttpServerBuilder::with_config(ServerConfig config) {
    config_ = std::move(config);
    return *this;
}

HttpServerBuilder& HttpServerBuilder::with_router(std::shared_ptr<http::Router> router) {
    router_ = std::move(router);
    return *this;
}

HttpServerBuilder& HttpServerBuilder::with_auth_service(std::shared_ptr<auth::AuthService> auth_service) {
    auth_service_ = std::move(auth_service);
    return *this;
}

HttpServerRuntime HttpServerBuilder::build() const {
    if (router_ == nullptr) {
        throw std::invalid_argument("http_server build rejected: error=router_missing");
    }
    if (auth_service_ == nullptr && router_->require_user_route_count() > 0U) {
        throw std::invalid_argument("http_server build rejected: error=auth_service_missing");
    }
    return {config_, router_, auth_service_};
}

}  // namespace nebula::server
