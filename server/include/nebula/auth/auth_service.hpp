#ifndef NEBULA_AUTH_AUTH_SERVICE_HPP
#define NEBULA_AUTH_AUTH_SERVICE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "nebula/auth/jwt_service.hpp"
#include "nebula/auth/password_hasher.hpp"
#include "nebula/user/user_repository.hpp"

namespace nebula::auth {

enum class AuthErrorCode : std::uint8_t {
    Ok,
    InvalidUsername,
    InvalidPassword,
    UsernameAlreadyExists,
    InvalidCredentials,
    TokenMissing,
    TokenInvalid,
    TokenExpired,
    InternalError,
};

[[nodiscard]] std::string_view to_string(AuthErrorCode error) noexcept;

struct RegisterResult {
    AuthErrorCode error = AuthErrorCode::InternalError;
    user::UserInfo user;
    std::string access_token;
};

struct LoginResult {
    AuthErrorCode error = AuthErrorCode::InternalError;
    user::UserInfo user;
    std::string access_token;
};

struct AuthenticateResult {
    AuthErrorCode error = AuthErrorCode::InternalError;
    user::UserInfo user;
    TokenClaims claims;
};

class AuthService {
public:
    AuthService(PasswordHasher password_hasher, JwtService jwt_service);

    [[nodiscard]] RegisterResult register_user(std::string_view username, std::string_view password);
    [[nodiscard]] LoginResult login(std::string_view username, std::string_view password) const;
    [[nodiscard]] AuthenticateResult authenticate_access_token(std::string_view access_token) const;

    [[nodiscard]] static bool is_valid_username(std::string_view username);
    [[nodiscard]] static bool is_valid_password(std::string_view password);

private:
    [[nodiscard]] static std::int64_t now_epoch_s();

    PasswordHasher password_hasher_;
    JwtService jwt_service_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_AUTH_SERVICE_HPP
