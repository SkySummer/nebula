#include "nebula/auth/domain/permissions.hpp"

namespace nebula::auth {

bool can_read_users(const UserProfile& actor) noexcept {
    return actor.role == UserRole::Owner || actor.role == UserRole::Admin;
}

bool can_manage_user(const UserProfile& actor, const UserProfile& target,
                     const std::optional<UserRole>& requested_role) noexcept {
    if (actor.role == UserRole::Owner) {
        return true;
    }
    if (actor.role != UserRole::Admin) {
        return false;
    }
    if (target.role == UserRole::Owner) {
        return false;
    }
    return !requested_role.has_value() || *requested_role != UserRole::Owner;
}

}  // namespace nebula::auth
