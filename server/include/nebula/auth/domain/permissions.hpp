#ifndef NEBULA_AUTH_DOMAIN_PERMISSIONS_HPP
#define NEBULA_AUTH_DOMAIN_PERMISSIONS_HPP

#include <optional>

#include "nebula/auth/domain/user.hpp"

namespace nebula::auth {

[[nodiscard]] bool can_read_users(const UserProfile& actor) noexcept;

[[nodiscard]] bool can_manage_user(const UserProfile& actor, const UserProfile& target,
                                   const std::optional<UserRole>& requested_role) noexcept;

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_DOMAIN_PERMISSIONS_HPP
