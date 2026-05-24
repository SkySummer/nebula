#ifndef NEBULA_AUTH_BOOTSTRAP_MODULE_HPP
#define NEBULA_AUTH_BOOTSTRAP_MODULE_HPP

#include <memory>

#include "nebula/auth/application/service.hpp"
#include "nebula/auth/bootstrap/config.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula/http/routing/router.hpp"

namespace nebula::auth {

class AuthModule {
public:
    struct Params {
        const AuthConfig* config;
        std::shared_ptr<database::ConnectionPool> database_pool;
        std::shared_ptr<http::Router> router;
    };

    [[nodiscard]] static std::unique_ptr<AuthModule> create(Params params) noexcept;

    [[nodiscard]] const std::shared_ptr<AuthService>& service() const noexcept;

private:
    AuthModule(std::shared_ptr<AuthRepository> repository, std::shared_ptr<AuthService> service);

    std::shared_ptr<AuthRepository> repository_;
    std::shared_ptr<AuthService> service_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_BOOTSTRAP_MODULE_HPP
