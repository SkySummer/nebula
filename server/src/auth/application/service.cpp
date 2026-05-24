#include "nebula/auth/application/service.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "nebula/auth/domain/permissions.hpp"
#include "nebula/common/platform/time.hpp"

namespace nebula::auth {

namespace {

bool is_username_char(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
}

}  // namespace

std::string_view to_string(AuthError error) noexcept {
    switch (error) {
        case AuthError::InvalidUsername:
            return "invalid_username";
        case AuthError::InvalidPassword:
            return "invalid_password";
        case AuthError::InvalidCredentials:
            return "invalid_credentials";
        case AuthError::UserAlreadyExists:
            return "user_already_exists";
        case AuthError::TokenMissing:
            return "token_missing";
        case AuthError::TokenInvalid:
            return "token_invalid";
        case AuthError::TokenExpired:
            return "token_expired";
        case AuthError::UserDisabled:
            return "user_disabled";
        case AuthError::Forbidden:
            return "forbidden";
        case AuthError::InvalidCurrentPassword:
            return "invalid_current_password";
        case AuthError::LastOwnerRequired:
            return "last_owner_required";
        case AuthError::UserNotFound:
            return "user_not_found";
        case AuthError::InternalError:
            return "internal_error";
    }
    std::unreachable();
}

AuthService::AuthService(std::shared_ptr<AuthRepository> repository, PasswordHasher password_hasher,
                         JwtService jwt_service)
    : repository_(std::move(repository)), password_hasher_(password_hasher), jwt_service_(std::move(jwt_service)) {
    if (repository_ == nullptr) {
        throw std::invalid_argument("auth_repository_missing");
    }
}

std::expected<AuthSessionResult, AuthError> AuthService::register_user(std::string_view username,
                                                                       std::string_view password) {
    if (!is_valid_username(username)) {
        return std::unexpected(AuthError::InvalidUsername);
    }
    if (!is_valid_password(password)) {
        return std::unexpected(AuthError::InvalidPassword);
    }

    const std::optional<PasswordHashValue> password_hash = password_hasher_.hash_password(password);
    if (!password_hash.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    auto created = repository_->create_user(username, *password_hash, common::now_epoch_s());
    if (!created.has_value()) {
        switch (created.error()) {
            case UserCreateError::DuplicateUsername:
                return std::unexpected(AuthError::UserAlreadyExists);
            case UserCreateError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    const std::optional<std::string> token =
        jwt_service_.issue_access_token(created->profile.user_id, created->token_version);
    if (!token.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    return AuthSessionResult{
        .user = created->profile,
        .access_token = *token,
    };
}

std::expected<AuthSessionResult, AuthError> AuthService::login_user(std::string_view username,
                                                                    std::string_view password) const {
    if (!is_valid_username(username)) {
        return std::unexpected(AuthError::InvalidUsername);
    }
    if (!is_valid_password(password)) {
        return std::unexpected(AuthError::InvalidPassword);
    }

    auto found_user = repository_->find_user_by_username(username);
    if (!found_user.has_value()) {
        switch (found_user.error()) {
            case UserFindError::NotFound:
                return std::unexpected(AuthError::InvalidCredentials);
            case UserFindError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    if (!PasswordHasher::verify_password(password, found_user->password_hash)) {
        return std::unexpected(AuthError::InvalidCredentials);
    }

    if (found_user->profile.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }

    const std::optional<std::string> token =
        jwt_service_.issue_access_token(found_user->profile.user_id, found_user->token_version);
    if (!token.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    return AuthSessionResult{
        .user = found_user->profile,
        .access_token = *token,
    };
}

std::expected<AuthenticateResult, AuthError> AuthService::authenticate_access_token(
    std::string_view access_token) const {
    if (access_token.empty()) {
        return std::unexpected(AuthError::TokenMissing);
    }

    const auto claims = jwt_service_.verify_access_token(access_token);
    if (!claims.has_value()) {
        switch (claims.error()) {
            case JwtVerifyError::Invalid:
                return std::unexpected(AuthError::TokenInvalid);
            case JwtVerifyError::Expired:
                return std::unexpected(AuthError::TokenExpired);
        }
    }

    auto found_user = repository_->find_user_by_id(claims->user_id);
    if (!found_user.has_value()) {
        switch (found_user.error()) {
            case UserFindError::NotFound:
                return std::unexpected(AuthError::TokenInvalid);
            case UserFindError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    if (claims->token_version != found_user->token_version) {
        return std::unexpected(AuthError::TokenInvalid);
    }
    if (found_user->profile.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }

    return AuthenticateResult{
        .user = found_user->profile,
        .claims = *claims,
    };
}

std::expected<AuthSessionResult, AuthError> AuthService::change_password(const UserProfile& actor,
                                                                         std::string_view current_password,
                                                                         std::string_view new_password) const {
    if (actor.user_id <= 0) {
        return std::unexpected(AuthError::TokenInvalid);
    }
    if (actor.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }
    if (!is_valid_password(current_password)) {
        return std::unexpected(AuthError::InvalidCurrentPassword);
    }
    if (!is_valid_password(new_password)) {
        return std::unexpected(AuthError::InvalidPassword);
    }

    auto found_user = repository_->find_user_by_id(actor.user_id);
    if (!found_user.has_value()) {
        switch (found_user.error()) {
            case UserFindError::NotFound:
                return std::unexpected(AuthError::UserNotFound);
            case UserFindError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    if (found_user->profile.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }
    if (!PasswordHasher::verify_password(current_password, found_user->password_hash)) {
        return std::unexpected(AuthError::InvalidCurrentPassword);
    }

    const std::optional<PasswordHashValue> password_hash = password_hasher_.hash_password(new_password);
    if (!password_hash.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    const std::int64_t next_token_version = found_user->token_version + 1;
    auto updated = repository_->update_password_hash(actor.user_id, *password_hash, next_token_version);
    if (!updated.has_value()) {
        switch (updated.error()) {
            case UserPasswordUpdateError::NotFound:
                return std::unexpected(AuthError::UserNotFound);
            case UserPasswordUpdateError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    const std::optional<std::string> token =
        jwt_service_.issue_access_token(updated->profile.user_id, updated->token_version);
    if (!token.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    return AuthSessionResult{
        .user = updated->profile,
        .access_token = *token,
    };
}

std::expected<ListUsersResult, AuthError> AuthService::list_users(const UserProfile& actor, std::int64_t limit,
                                                                  std::int64_t offset) const {
    if (actor.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }
    if (!can_read_users(actor)) {
        return std::unexpected(AuthError::Forbidden);
    }

    auto listed = repository_->list_users(limit, offset);
    if (!listed.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    return ListUsersResult{
        .users = listed->users,
        .limit = listed->limit,
        .offset = listed->offset,
        .has_more = listed->has_more,
    };
}

std::expected<UserProfile, AuthError> AuthService::get_user(const UserProfile& actor, std::int64_t user_id) const {
    if (actor.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }
    if (!can_read_users(actor)) {
        return std::unexpected(AuthError::Forbidden);
    }

    auto found_user = repository_->find_user_by_id(user_id);
    if (!found_user.has_value()) {
        switch (found_user.error()) {
            case UserFindError::NotFound:
                return std::unexpected(AuthError::UserNotFound);
            case UserFindError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    return found_user->profile;
}

std::expected<UserProfile, AuthError> AuthService::update_user(const UserProfile& actor, std::int64_t user_id,
                                                               std::optional<UserRole> role,
                                                               std::optional<UserStatus> status) const {
    if (actor.status == UserStatus::Disabled) {
        return std::unexpected(AuthError::UserDisabled);
    }
    if (!role.has_value() && !status.has_value()) {
        return std::unexpected(AuthError::InternalError);
    }

    auto found_user = repository_->find_user_by_id(user_id);
    if (!found_user.has_value()) {
        switch (found_user.error()) {
            case UserFindError::NotFound:
                return std::unexpected(AuthError::UserNotFound);
            case UserFindError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    if (!can_manage_user(actor, found_user->profile, role)) {
        return std::unexpected(AuthError::Forbidden);
    }

    auto updated = repository_->update_user(user_id, role, status);
    if (!updated.has_value()) {
        switch (updated.error()) {
            case UserRoleStatusUpdateError::NotFound:
                return std::unexpected(AuthError::UserNotFound);
            case UserRoleStatusUpdateError::LastOwnerRequired:
                return std::unexpected(AuthError::LastOwnerRequired);
            case UserRoleStatusUpdateError::InternalError:
                return std::unexpected(AuthError::InternalError);
        }
    }

    return *updated;
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

}  // namespace nebula::auth
