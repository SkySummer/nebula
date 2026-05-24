#include "nebula/auth/bootstrap/module.hpp"

#include <memory>
#include <optional>

#include "nebula/auth/http/routes.hpp"
#include "nebula/auth/infra/jwt_secret_store.hpp"
#include "nebula/auth/repository/repository.hpp"
#include "nebula/common/log/logger.hpp"

namespace nebula::auth {

namespace {

void log_auth_module_init_failed(std::string_view error) {
    common::Logger::instance().fatal("auth module init failed").field("error", error).field("decision", "return_null");
}

}  // namespace

AuthModule::AuthModule(std::shared_ptr<AuthRepository> repository, std::shared_ptr<AuthService> service)
    : repository_(std::move(repository)), service_(std::move(service)) {}

std::unique_ptr<AuthModule> AuthModule::create(Params params) noexcept {
    if (params.config == nullptr) {
        log_auth_module_init_failed("config_missing");
        return nullptr;
    }
    if (params.database_pool == nullptr) {
        log_auth_module_init_failed("database_pool_missing");
        return nullptr;
    }
    if (params.router == nullptr) {
        log_auth_module_init_failed("router_missing");
        return nullptr;
    }

    try {
        auto repository = std::make_shared<AuthRepository>(std::move(params.database_pool));
        if (!repository->check_schema_ready()) {
            log_auth_module_init_failed("auth_repository_not_ready");
            return nullptr;
        }

        auto jwt_secret = load_or_create_jwt_secret(params.config->jwt_secret_path);
        if (!jwt_secret.has_value()) {
            log_auth_module_init_failed(to_string(jwt_secret.error()));
            return nullptr;
        }

        auto service = std::make_shared<AuthService>(
            repository, PasswordHasher({.iterations = params.config->password_hash_iterations}),
            JwtService({.secret = std::move(*jwt_secret), .access_token_ttl_s = params.config->access_token_ttl_s}));

        if (!register_auth_routes(params.router, service)) {
            log_auth_module_init_failed("register_auth_routes_failed");
            return nullptr;
        }

        return std::unique_ptr<AuthModule>(new AuthModule(std::move(repository), std::move(service)));
    } catch (const std::exception& e) {
        log_auth_module_init_failed(e.what());
        return nullptr;
    } catch (...) {
        log_auth_module_init_failed("unknown");
        return nullptr;
    }
}

const std::shared_ptr<AuthService>& AuthModule::service() const noexcept {
    return service_;
}

}  // namespace nebula::auth
