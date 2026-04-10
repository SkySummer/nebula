#include "nebula/auth/auth_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <utility>

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
    const user::UserFindResult existing = user::find_user_by_username(username);
    if (existing.status == user::UserFindStatus::InternalError) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }
    if (existing.status == user::UserFindStatus::Found) {
        result.error = AuthErrorCode::UsernameAlreadyExists;
        return result;
    }

    const std::optional<std::string> password_hash = password_hasher_.hash_password(password);
    if (!password_hash.has_value()) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    const user::AllocateIdResult allocated = user::allocate_user_id();
    if (allocated.status != user::UserAllocateIdStatus::Allocated) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    user::UserInfo user_info;
    user_info.user_id = allocated.user_id;
    user_info.username = std::string(username);
    user_info.password_hash = *password_hash;
    user_info.created_at_s = now_epoch_s();

    const std::optional<std::string> token = jwt_service_.issue_access_token(user_info.user_id, user_info.created_at_s);
    if (!token.has_value()) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    const user::UserCreateResult create_result = user::create_user(user_info);
    if (create_result == user::UserCreateResult::DuplicateUsername) {
        result.error = AuthErrorCode::UsernameAlreadyExists;
        return result;
    }
    if (create_result == user::UserCreateResult::InternalError) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }

    result.error = AuthErrorCode::Ok;
    result.user = std::move(user_info);
    result.access_token = *token;
    return result;
}

LoginResult AuthService::login(std::string_view username, std::string_view password) const {
    LoginResult result;

    if (!is_valid_username(username)) {
        result.error = AuthErrorCode::InvalidUsername;
        return result;
    }
    if (!is_valid_password(password)) {
        result.error = AuthErrorCode::InvalidPassword;
        return result;
    }
    const user::UserFindResult found_user = user::find_user_by_username(username);
    if (found_user.status == user::UserFindStatus::InternalError) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }
    if (found_user.status == user::UserFindStatus::NotFound) {
        result.error = AuthErrorCode::InvalidCredentials;
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
    if (verify_result == JwtVerifyResult::Invalid) {
        result.error = AuthErrorCode::TokenInvalid;
        return result;
    }
    if (verify_result == JwtVerifyResult::Expired) {
        result.error = AuthErrorCode::TokenExpired;
        return result;
    }

    const user::UserFindResult found_user = user::find_user_by_id(claims.user_id);
    if (found_user.status == user::UserFindStatus::InternalError) {
        result.error = AuthErrorCode::InternalError;
        return result;
    }
    if (found_user.status == user::UserFindStatus::NotFound) {
        result.error = AuthErrorCode::TokenInvalid;
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

std::int64_t AuthService::now_epoch_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace nebula::auth
