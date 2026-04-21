#include "nebula/auth/auth_service.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include "nebula/app/server_config.hpp"
#include "nebula/auth/jwt_secret_store.hpp"
#include "nebula/auth/user_repository.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/time_utils.hpp"

namespace nebula::auth {

namespace {

bool is_username_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

}  // namespace

std::string_view to_string(AuthErrorCode error) noexcept {
    switch (error) {
        case AuthErrorCode::Ok:
            return "ok";
        case AuthErrorCode::InvalidUsername:
            return "invalid_username";
        case AuthErrorCode::InvalidPassword:
            return "invalid_password";
        case AuthErrorCode::UsernameAlreadyExists:
            return "username_already_exists";
        case AuthErrorCode::InvalidCredentials:
            return "invalid_credentials";
        case AuthErrorCode::TokenMissing:
            return "token_missing";
        case AuthErrorCode::TokenInvalid:
            return "token_invalid";
        case AuthErrorCode::TokenExpired:
            return "token_expired";
        case AuthErrorCode::InternalError:
            return "internal_error";
    }
    return "unknown";
}

AuthService::AuthService(PasswordHasher password_hasher, JwtService jwt_service)
    : password_hasher_(password_hasher), jwt_service_(std::move(jwt_service)) {}

RegisterResult AuthService::register_user(std::string_view username, std::string_view password) {
    RegisterResult result;

    if (!is_valid_username(username)) {
        result.error = AuthErrorCode::InvalidUsername;
        return result;
    }
    if (!is_valid_password(password)) {
        result.error = AuthErrorCode::InvalidPassword;
        return result;
    }

    const UserFindResult existing = find_user_by_username(username);
    switch (existing.status) {
        case UserFindStatus::Found:
            result.error = AuthErrorCode::UsernameAlreadyExists;
            return result;
        case UserFindStatus::NotFound:
            break;
        case UserFindStatus::InternalError:
            result.error = AuthErrorCode::InternalError;
            return result;
    }

    const std::optional<PasswordHashValue> password_hash = password_hasher_.hash_password(password);
    if (!password_hash.has_value()) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    const UserAllocateIdResult allocated = allocate_user_id();
    if (allocated.status != UserAllocateIdStatus::Allocated) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    UserInfo user_info;
    user_info.user_id = allocated.user_id;
    user_info.username = std::string(username);
    user_info.password_hash = *password_hash;
    user_info.created_at_s = common::now_epoch_s();

    const std::optional<std::string> token = jwt_service_.issue_access_token(user_info.user_id, user_info.created_at_s);
    if (!token.has_value()) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    const UserCreateResult create_result = create_user(user_info);
    switch (create_result) {
        case UserCreateResult::Created:
            break;
        case UserCreateResult::DuplicateUsername:
            result.error = AuthErrorCode::UsernameAlreadyExists;
            return result;
        case UserCreateResult::InternalError:
            result.error = AuthErrorCode::InternalError;
            return result;
    }

    result.error = AuthErrorCode::Ok;
    result.user = std::move(user_info);
    result.access_token = *token;
    return result;
}

LoginResult AuthService::login_user(std::string_view username, std::string_view password) const {
    LoginResult result;

    if (!is_valid_username(username)) {
        result.error = AuthErrorCode::InvalidUsername;
        return result;
    }
    if (!is_valid_password(password)) {
        result.error = AuthErrorCode::InvalidPassword;
        return result;
    }

    const UserFindResult found_user = find_user_by_username(username);
    switch (found_user.status) {
        case UserFindStatus::Found:
            break;
        case UserFindStatus::NotFound:
            result.error = AuthErrorCode::InvalidCredentials;
            return result;
        case UserFindStatus::InternalError:
            result.error = AuthErrorCode::InternalError;
            return result;
    }

    if (!PasswordHasher::verify_password(password, found_user.info.password_hash)) {
        result.error = AuthErrorCode::InvalidCredentials;
        return result;
    }

    const std::optional<std::string> token = jwt_service_.issue_access_token(found_user.info.user_id);
    if (!token.has_value()) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    result.error = AuthErrorCode::Ok;
    result.user = found_user.info;
    result.access_token = *token;
    return result;
}

AuthenticateResult AuthService::authenticate_access_token(std::string_view access_token) const {
    AuthenticateResult result;

    if (access_token.empty()) {
        result.error = AuthErrorCode::TokenMissing;
        return result;
    }

    TokenClaims claims;
    const JwtVerifyResult verify_result = jwt_service_.verify_access_token(access_token, claims);
    switch (verify_result) {
        case JwtVerifyResult::Valid:
            break;
        case JwtVerifyResult::Invalid:
            result.error = AuthErrorCode::TokenInvalid;
            return result;
        case JwtVerifyResult::Expired:
            result.error = AuthErrorCode::TokenExpired;
            return result;
    }

    const UserFindResult found_user = find_user_by_id(claims.user_id);
    switch (found_user.status) {
        case UserFindStatus::Found:
            break;
        case UserFindStatus::NotFound:
            result.error = AuthErrorCode::TokenInvalid;
            return result;
        case UserFindStatus::InternalError:
            result.error = AuthErrorCode::InternalError;
            return result;
    }

    result.error = AuthErrorCode::Ok;
    result.user = found_user.info;
    result.claims = claims;
    return result;
}

bool AuthService::is_valid_username(std::string_view username) {
    if (username.size() < 3U || username.size() > 32U) {
        return false;
    }
    return std::ranges::all_of(username, is_username_char);
}

bool AuthService::is_valid_password(std::string_view password) {
    return password.size() >= 8U && password.size() <= 72U;
}

std::shared_ptr<AuthService> initialize_auth_service(const app::ServerConfig& config) {
    const std::optional<std::string> jwt_secret = load_or_create_jwt_secret(config.auth_jwt_secret_path);
    if (!jwt_secret.has_value()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "auth service init failed")
            .field("path", config.auth_jwt_secret_path.string())
            .field("error", "jwt_secret_init_failed")
            .field("decision", "exit_process");
        return nullptr;
    }

    return std::make_shared<AuthService>(
        PasswordHasher({.iterations = config.auth_password_hash_iterations}),
        JwtService({.secret = *jwt_secret, .access_token_ttl_s = config.auth_access_token_ttl_s}));
}

}  // namespace nebula::auth
