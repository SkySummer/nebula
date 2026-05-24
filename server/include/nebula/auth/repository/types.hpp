#ifndef NEBULA_AUTH_REPOSITORY_TYPES_HPP
#define NEBULA_AUTH_REPOSITORY_TYPES_HPP

#include <cstdint>
#include <vector>

#include "nebula/auth/domain/user.hpp"

namespace nebula::auth {

inline constexpr std::int64_t kGlobalUsersMutationAdvisoryLockKey = 0x5b18'6e80'68b2'3cf9;

enum class UserCreateError : std::uint8_t {
    DuplicateUsername,
    InternalError,
};

enum class UserPasswordUpdateError : std::uint8_t {
    NotFound,
    InternalError,
};

enum class UserFindError : std::uint8_t {
    NotFound,
    InternalError,
};

enum class UserRoleStatusUpdateError : std::uint8_t {
    NotFound,
    LastOwnerRequired,
    InternalError,
};

struct UserListPage {
    std::vector<UserProfile> users;
    std::int64_t limit = 0;
    std::int64_t offset = 0;
    bool has_more = false;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_REPOSITORY_TYPES_HPP
