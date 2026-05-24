#ifndef NEBULA_AUTH_APPLICATION_SERVICE_HPP
#define NEBULA_AUTH_APPLICATION_SERVICE_HPP

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/auth/domain/error.hpp"
#include "nebula/auth/domain/user.hpp"
#include "nebula/auth/infra/jwt_service.hpp"
#include "nebula/auth/infra/password_hasher.hpp"
#include "nebula/auth/repository/repository.hpp"

namespace nebula::auth {

struct AuthSessionResult {
    UserProfile user;
    std::string access_token;
};

struct AuthenticateResult {
    UserProfile user;
    TokenClaims claims;
};

struct ListUsersResult {
    std::vector<UserProfile> users;
    std::int64_t limit = 0;
    std::int64_t offset = 0;
    bool has_more = false;
};

class AuthService {
public:
    AuthService(std::shared_ptr<AuthRepository> repository, PasswordHasher password_hasher, JwtService jwt_service);

    [[nodiscard]] std::expected<AuthSessionResult, AuthError> register_user(std::string_view username,
                                                                            std::string_view password);
    [[nodiscard]] std::expected<AuthSessionResult, AuthError> login_user(std::string_view username,
                                                                         std::string_view password) const;
    [[nodiscard]] std::expected<AuthenticateResult, AuthError> authenticate_access_token(
        std::string_view access_token) const;
    [[nodiscard]] std::expected<AuthSessionResult, AuthError> change_password(const UserProfile& actor,
                                                                              std::string_view current_password,
                                                                              std::string_view new_password) const;
    [[nodiscard]] std::expected<ListUsersResult, AuthError> list_users(const UserProfile& actor, std::int64_t limit,
                                                                       std::int64_t offset) const;
    [[nodiscard]] std::expected<UserProfile, AuthError> get_user(const UserProfile& actor, std::int64_t user_id) const;
    [[nodiscard]] std::expected<UserProfile, AuthError> update_user(const UserProfile& actor, std::int64_t user_id,
                                                                    std::optional<UserRole> role,
                                                                    std::optional<UserStatus> status) const;

    [[nodiscard]] static bool is_valid_username(std::string_view username);
    [[nodiscard]] static bool is_valid_password(std::string_view password);

private:
    std::shared_ptr<AuthRepository> repository_;
    PasswordHasher password_hasher_;
    JwtService jwt_service_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_APPLICATION_SERVICE_HPP
