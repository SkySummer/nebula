#ifndef NEBULA_AUTH_AUTH_SERVICE_HPP
#define NEBULA_AUTH_AUTH_SERVICE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "nebula/auth/jwt_service.hpp"
#include "nebula/auth/password_hasher.hpp"
#include "nebula/auth/user_types.hpp"
#include "nebula/server/server_config.hpp"

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
    UserInfo user;
    std::string access_token;
};

struct LoginResult {
    AuthErrorCode error = AuthErrorCode::InternalError;
    UserInfo user;
    std::string access_token;
};

struct AuthenticateResult {
    AuthErrorCode error = AuthErrorCode::InternalError;
    UserInfo user;
    TokenClaims claims;
};

class AuthService {
public:
    AuthService(PasswordHasher password_hasher, JwtService jwt_service);

    [[nodiscard]] RegisterResult register_user(std::string_view username, std::string_view password);
    [[nodiscard]] LoginResult login_user(std::string_view username, std::string_view password) const;
    [[nodiscard]] AuthenticateResult authenticate_access_token(std::string_view access_token) const;

    [[nodiscard]] static bool is_valid_username(std::string_view username);
    [[nodiscard]] static bool is_valid_password(std::string_view password);

private:
    PasswordHasher password_hasher_;
    JwtService jwt_service_;
};

[[nodiscard]] std::shared_ptr<AuthService> initialize_auth_service(const server::ServerConfig& config);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_AUTH_SERVICE_HPP
