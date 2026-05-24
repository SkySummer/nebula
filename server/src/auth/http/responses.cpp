#include "nebula/auth/http/responses.hpp"

#include <utility>

#include "nebula/http/codec/response_writer.hpp"

namespace nebula::auth {

http::HttpResponse to_http_response(AuthError error) {
    switch (error) {
        case AuthError::InvalidUsername:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_username", "invalid username");
        case AuthError::InvalidPassword:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_password", "invalid password");
        case AuthError::InvalidCredentials:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "invalid_credentials",
                                                 "invalid credentials");
        case AuthError::UserAlreadyExists:
            return http::make_api_error_response(http::HttpStatus::Conflict, "user_already_exists",
                                                 "username already exists");
        case AuthError::TokenMissing:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_missing",
                                                 "missing bearer token");
        case AuthError::TokenInvalid:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_invalid",
                                                 "invalid access token");
        case AuthError::TokenExpired:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_expired",
                                                 "access token expired");
        case AuthError::UserDisabled:
            return http::make_api_error_response(http::HttpStatus::Forbidden, "user_disabled", "user is disabled");
        case AuthError::Forbidden:
            return http::make_api_error_response(http::HttpStatus::Forbidden, "forbidden", "forbidden");
        case AuthError::InvalidCurrentPassword:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "invalid_current_password",
                                                 "invalid current password");
        case AuthError::LastOwnerRequired:
            return http::make_api_error_response(http::HttpStatus::Conflict, "last_owner_required",
                                                 "at least one active owner is required");
        case AuthError::UserNotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "user_not_found", "user not found");
        case AuthError::InternalError:
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }
    std::unreachable();
}

}  // namespace nebula::auth
