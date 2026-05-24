#include "nebula/auth/domain/user.hpp"

#include <utility>

namespace nebula::auth {

std::string_view to_string(UserRole role) noexcept {
    switch (role) {
        case UserRole::Owner:
            return "owner";
        case UserRole::Admin:
            return "admin";
        case UserRole::User:
            return "user";
    }
    std::unreachable();
}

std::optional<UserRole> parse_user_role(std::string_view text) noexcept {
    if (text == "owner") {
        return UserRole::Owner;
    }
    if (text == "admin") {
        return UserRole::Admin;
    }
    if (text == "user") {
        return UserRole::User;
    }
    return std::nullopt;
}

std::string_view to_string(UserStatus status) noexcept {
    switch (status) {
        case UserStatus::Active:
            return "active";
        case UserStatus::Disabled:
            return "disabled";
    }
    std::unreachable();
}

std::optional<UserStatus> parse_user_status(std::string_view text) noexcept {
    if (text == "active") {
        return UserStatus::Active;
    }
    if (text == "disabled") {
        return UserStatus::Disabled;
    }
    return std::nullopt;
}

}  // namespace nebula::auth
