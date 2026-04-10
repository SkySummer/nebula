#ifndef NEBULA_USER_USER_REPOSITORY_HPP
#define NEBULA_USER_USER_REPOSITORY_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace nebula::user {

struct UserInfo {
    std::int64_t user_id = 0;
    std::string username;
    std::string password_hash;
    std::int64_t created_at_s = 0;
};

enum class UserAllocateIdStatus : std::uint8_t {
    Allocated,
    InternalError,
};

struct AllocateIdResult {
    UserAllocateIdStatus status = UserAllocateIdStatus::InternalError;
    std::int64_t user_id = 0;
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

[[nodiscard]] bool check_user_schema_ready();

[[nodiscard]] AllocateIdResult allocate_user_id();
UserCreateResult create_user(const UserInfo& info);
[[nodiscard]] UserFindResult find_user_by_username(std::string_view username);
[[nodiscard]] UserFindResult find_user_by_id(std::int64_t user_id);

}  // namespace nebula::user

#endif  // NEBULA_USER_USER_REPOSITORY_HPP
