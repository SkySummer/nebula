#ifndef NEBULA_AUTH_DOMAIN_USER_HPP
#define NEBULA_AUTH_DOMAIN_USER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "nebula/auth/domain/password_hash.hpp"

namespace nebula::auth {

enum class UserRole : std::uint8_t {
    Owner,
    Admin,
    User,
};

[[nodiscard]] std::string_view to_string(UserRole role) noexcept;

[[nodiscard]] std::optional<UserRole> parse_user_role(std::string_view text) noexcept;

enum class UserStatus : std::uint8_t {
    Active,
    Disabled,
};

[[nodiscard]] std::string_view to_string(UserStatus status) noexcept;

[[nodiscard]] std::optional<UserStatus> parse_user_status(std::string_view text) noexcept;

struct UserProfile {
    std::int64_t user_id = 0;
    std::string username;
    UserRole role = UserRole::User;
    UserStatus status = UserStatus::Active;
    std::int64_t created_at_s = 0;
};

struct UserAuthRecord {
    UserProfile profile;
    PasswordHashValue password_hash;
    std::int64_t token_version = 1;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_DOMAIN_USER_HPP
