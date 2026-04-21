#ifndef NEBULA_AUTH_USER_TYPES_HPP
#define NEBULA_AUTH_USER_TYPES_HPP

#include <cstdint>
#include <string>

#include "nebula/auth/password_hasher.hpp"

namespace nebula::auth {

struct UserInfo {
    std::int64_t user_id = 0;
    std::string username;
    PasswordHashValue password_hash;
    std::int64_t created_at_s = 0;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_USER_TYPES_HPP
