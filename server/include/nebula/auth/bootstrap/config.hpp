#ifndef NEBULA_AUTH_BOOTSTRAP_CONFIG_HPP
#define NEBULA_AUTH_BOOTSTRAP_CONFIG_HPP

#include <cstdint>
#include <filesystem>

#include "nebula/auth/domain/limits.hpp"

namespace nebula::auth {

struct AuthConfig {
    std::filesystem::path jwt_secret_path = "runtime/secrets/jwt.key";
    std::int64_t access_token_ttl_s = kDefaultAccessTokenTtlSeconds;
    std::uint32_t password_hash_iterations = kDefaultPasswordHashIterations;

    [[nodiscard]] bool validate() const;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_BOOTSTRAP_CONFIG_HPP
