#ifndef NEBULA_AUTH_INFRA_PASSWORD_HASHER_HPP
#define NEBULA_AUTH_INFRA_PASSWORD_HASHER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/auth/domain/password_hash.hpp"

namespace nebula::auth {

struct PasswordHashConfig {
    std::uint32_t iterations = kDefaultPasswordHashIterations;
    std::size_t salt_bytes = kDefaultPasswordHashSaltBytes;
    std::size_t derived_key_bytes = kDefaultPasswordHashDerivedKeyBytes;
};

class PasswordHasher {
public:
    explicit PasswordHasher(PasswordHashConfig config = {});

    [[nodiscard]] std::optional<PasswordHashValue> hash_password(std::string_view password) const;

    [[nodiscard]] static bool verify_password(std::string_view password, const PasswordHashValue& value);

private:
    PasswordHashConfig config_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_INFRA_PASSWORD_HASHER_HPP
