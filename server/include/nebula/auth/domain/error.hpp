#ifndef NEBULA_AUTH_DOMAIN_ERROR_HPP
#define NEBULA_AUTH_DOMAIN_ERROR_HPP

#include <cstdint>
#include <string_view>

namespace nebula::auth {

enum class AuthError : std::uint8_t {
    InvalidUsername,
    InvalidPassword,
    InvalidCredentials,
    UserAlreadyExists,
    TokenMissing,
    TokenInvalid,
    TokenExpired,
    UserDisabled,
    Forbidden,
    InvalidCurrentPassword,
    LastOwnerRequired,
    UserNotFound,
    InternalError,
};

[[nodiscard]] std::string_view to_string(AuthError error) noexcept;

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_DOMAIN_ERROR_HPP
