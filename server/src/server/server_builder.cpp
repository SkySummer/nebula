#include "nebula/server/server_builder.hpp"

#include <stdexcept>
#include <utility>

namespace nebula::server {

ServerBuilder& ServerBuilder::with_config(app::ServerConfig config) {
    config_ = std::move(config);
    return *this;
}

ServerBuilder& ServerBuilder::with_router(std::shared_ptr<http::Router> router) {
    router_ = std::move(router);
    return *this;
}

ServerBuilder& ServerBuilder::with_auth_service(std::shared_ptr<auth::AuthService> auth_service) {
    auth_service_ = std::move(auth_service);
    return *this;
}

ServerRuntime ServerBuilder::build() const {
    if (router_ == nullptr) {
        throw std::invalid_argument("http_server build rejected: error=router_missing");
    }
    if (auth_service_ == nullptr && router_->require_user_route_count() > 0U) {
        throw std::invalid_argument("http_server build rejected: error=auth_service_missing");
    }
    return {config_, router_, auth_service_};
}

}  // namespace nebula::server
