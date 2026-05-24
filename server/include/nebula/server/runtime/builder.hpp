#ifndef NEBULA_SERVER_RUNTIME_BUILDER_HPP
#define NEBULA_SERVER_RUNTIME_BUILDER_HPP

#include <memory>

#include "nebula/app/app_config.hpp"
#include "nebula/auth/application/service.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/server/runtime/runtime.hpp"

namespace nebula::server {

class ServerBuilder {
public:
    ServerBuilder& with_config(const app::AppConfig& config);
    ServerBuilder& with_router(std::shared_ptr<http::Router> router);
    ServerBuilder& with_auth_service(std::shared_ptr<auth::AuthService> auth_service);
    [[nodiscard]] ServerRuntime build() const;

private:
    ServerConfig server_config_{};
    ServerTimeoutConfig timeouts_{};
    http::HttpLimitsConfig limits_{};
    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_RUNTIME_BUILDER_HPP
