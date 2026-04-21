#ifndef NEBULA_AUTH_USER_REPOSITORY_HPP
#define NEBULA_AUTH_USER_REPOSITORY_HPP

#include <cstdint>
#include <string_view>

#include "nebula/auth/user_types.hpp"

namespace nebula::auth {

enum class UserAllocateIdStatus : std::uint8_t {
    Allocated,
    InternalError,
};

enum class UserCreateResult : std::uint8_t {
    Created,
    DuplicateUsername,
    InternalError,
};

enum class UserFindStatus : std::uint8_t {
    Found,
    NotFound,
    InternalError,
};

struct UserFindResult {
    UserFindStatus status = UserFindStatus::InternalError;
    UserInfo info;
};

struct UserAllocateIdResult {
    UserAllocateIdStatus status = UserAllocateIdStatus::InternalError;
    std::int64_t user_id = 0;
};

[[nodiscard]] bool check_user_schema_ready();

[[nodiscard]] UserAllocateIdResult allocate_user_id();

UserCreateResult create_user(const UserInfo& info);

[[nodiscard]] UserFindResult find_user_by_username(std::string_view username);
[[nodiscard]] UserFindResult find_user_by_id(std::int64_t user_id);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_USER_REPOSITORY_HPP
