#include "nebula/auth/bootstrap/config.hpp"

#include "nebula/common/log/logger.hpp"

namespace nebula::auth {

bool AuthConfig::validate() const {
    bool ok = true;
    if (jwt_secret_path.empty()) {
        common::Logger::instance()
            .error("auth config value invalid")
            .field("key", "jwt_secret_path")
            .field("error", "empty_value");
        ok = false;
    }
    if (access_token_ttl_s < kMinAccessTokenTtlSeconds || access_token_ttl_s > kMaxAccessTokenTtlSeconds) {
        common::Logger::instance()
            .error("auth config value out of range")
            .field("key", "access_token_ttl_s")
            .field("value", access_token_ttl_s)
            .field("min_value", kMinAccessTokenTtlSeconds)
            .field("max_value", kMaxAccessTokenTtlSeconds);
        ok = false;
    }
    if (password_hash_iterations < kMinPasswordHashIterations ||
        password_hash_iterations > kMaxPasswordHashIterations) {
        common::Logger::instance()
            .error("auth config value out of range")
            .field("key", "password_hash_iterations")
            .field("value", password_hash_iterations)
            .field("min_value", kMinPasswordHashIterations)
            .field("max_value", kMaxPasswordHashIterations);
        ok = false;
    }
    return ok;
}

}  // namespace nebula::auth
