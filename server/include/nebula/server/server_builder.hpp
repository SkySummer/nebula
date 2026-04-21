#ifndef NEBULA_SERVER_SERVER_BUILDER_HPP
#define NEBULA_SERVER_SERVER_BUILDER_HPP

#include <memory>

#include "nebula/app/server_config.hpp"
#include "nebula/auth/auth_service.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/server_runtime.hpp"

namespace nebula::server {

class ServerBuilder {
public:
    ServerBuilder& with_config(app::ServerConfig config);
    ServerBuilder& with_router(std::shared_ptr<http::Router> router);
    ServerBuilder& with_auth_service(std::shared_ptr<auth::AuthService> auth_service);
    [[nodiscard]] ServerRuntime build() const;

private:
    app::ServerConfig config_{};
    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SERVER_BUILDER_HPP
