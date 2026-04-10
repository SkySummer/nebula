#ifndef NEBULA_AUTH_PASSWORD_HASHER_HPP
#define NEBULA_AUTH_PASSWORD_HASHER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nebula/auth/password_hash_limits.hpp"

namespace nebula::auth {

[[nodiscard]] std::string format_password_hash(std::uint64_t iterations, std::string_view salt,
                                               std::string_view derived_key);

struct PasswordHashConfig {
    std::uint32_t iterations = kDefaultPasswordHashIterations;
    std::size_t salt_bytes = kDefaultPasswordHashSaltBytes;
    std::size_t derived_key_bytes = kDefaultPasswordHashDerivedKeyBytes;
};

class PasswordHasher {
public:
    explicit PasswordHasher(PasswordHashConfig config = {});

    [[nodiscard]] std::optional<std::string> hash_password(std::string_view password) const;
    [[nodiscard]] static bool verify_password(std::string_view password, std::string_view encoded_hash);

private:
    PasswordHashConfig config_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_PASSWORD_HASHER_HPP
