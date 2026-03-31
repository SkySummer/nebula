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

HttpServerRuntime HttpServerBuilder::build() const {
    if (router_ == nullptr) {
        throw std::invalid_argument("http_server build rejected: error=router_missing");
    }
    return HttpServerRuntime(config_, router_);
}

}  // namespace nebula::server
