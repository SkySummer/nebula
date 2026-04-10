#include "nebula/auth/auth_http.hpp"

#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/auth/auth_service.hpp"
#include "nebula/auth/jwt_secret_store.hpp"
#include "nebula/common/json.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula/common/string_utils.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/http/router.hpp"
#include "nebula/user/user_repository.hpp"

namespace nebula::auth {

namespace {

bool starts_with_ignore_case_ascii(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < prefix.size(); ++idx) {
        const auto left = static_cast<unsigned char>(text[idx]);
        const auto right = static_cast<unsigned char>(prefix[idx]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

common::JsonValue user_json(const user::UserInfo& user) {
    common::JsonObject object;
    object.emplace("user_id", common::JsonValue(user.user_id));
    object.emplace("username", common::JsonValue(user.username));
    object.emplace("created_at_s", common::JsonValue(user.created_at_s));
    return common::JsonValue(std::move(object));
}

http::HttpResponse map_auth_error(AuthErrorCode error) {
    switch (error) {
        case AuthErrorCode::InvalidUsername:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_username", "invalid username");
        case AuthErrorCode::InvalidPassword:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_password", "invalid password");
        case AuthErrorCode::UsernameAlreadyExists:
            return http::make_api_error_response(http::HttpStatus::Conflict, "username_already_exists",
                                                 "username already exists");
        case AuthErrorCode::InvalidCredentials:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "invalid_credentials",
                                                 "invalid credentials");
        case AuthErrorCode::TokenMissing:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_missing",
                                                 "missing bearer token");
        case AuthErrorCode::TokenInvalid:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_invalid",
                                                 "invalid access token");
        case AuthErrorCode::TokenExpired:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "token_expired",
                                                 "access token expired");
        case AuthErrorCode::InternalError:
            return http::make_api_error_response(http::HttpStatus::InternalServerError, "internal_error",
                                                 "internal error");
        case AuthErrorCode::Ok:
            break;
    }
    return http::make_api_error_response(http::HttpStatus::InternalServerError, "internal_error", "internal error");
}

std::optional<std::pair<std::string, std::string>> parse_credentials(std::string_view body) {
    const common::JsonParseResult parsed = common::parse_json(body);
    if (!parsed.ok) {
        return std::nullopt;
    }

    const common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }

    for (const auto& entry : *object) {
        if (!entry.second.is_string()) {
            return std::nullopt;
        }
    }

    const auto username_it = object->find("username");
    const auto password_it = object->find("password");
    if (username_it == object->end() || password_it == object->end()) {
        return std::nullopt;
    }

    const std::string* username = username_it->second.get_if_string();
    const std::string* password = password_it->second.get_if_string();
    if (username == nullptr || password == nullptr) {
        return std::nullopt;
    }

    return std::pair<std::string, std::string>{*username, *password};
}

std::string extract_bearer_token(const http::HeaderMap& headers) {
    const auto auth_it = headers.find("authorization");
    if (auth_it == headers.end()) {
        return {};
    }

    const std::string_view value = common::trim_ascii(auth_it->second);
    constexpr std::string_view k_prefix = "Bearer ";
    if (!starts_with_ignore_case_ascii(value, k_prefix)) {
        return {};
    }
    return std::string(common::trim_ascii(value.substr(k_prefix.size())));
}

bool register_route(const std::shared_ptr<http::Router>& router, http::HttpMethod method, std::string_view path,
                    http::Router::Handler handler) {
    const bool added = router->add_route(method, std::string(path), std::move(handler));
    if (added) {
        return true;
    }

    common::Logger::instance()
        .fatal(common::LogDomain::Auth, "register auth route failed")
        .field("method", http::to_string(method))
        .field("path", path)
        .field("error", "register_route_failed")
        .field("decision", "exit_process");
    return false;
}

std::optional<std::string> resolve_database_password(const server::ServerConfig& config) {
    if (!config.database_password_env.empty()) {
        const char* env_value = std::getenv(config.database_password_env.c_str());
        if (env_value != nullptr && *env_value != '\0') {
            return std::string(env_value);
        }
    }
    return std::nullopt;
}

http::HttpResponse handle_register(const std::shared_ptr<AuthService>& auth_service,
                                   const http::RouteContext& context) {
    const std::optional<std::pair<std::string, std::string>> credentials = parse_credentials(context.request.body);
    if (!credentials.has_value()) {
        common::Logger::instance()
            .warn(common::LogDomain::Auth, "register rejected")
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const RegisterResult registered = auth_service->register_user(credentials->first, credentials->second);
    if (registered.error != AuthErrorCode::Ok) {
        common::Logger::instance()
            .warn(common::LogDomain::Auth, "register rejected")
            .field("username", credentials->first)
            .field("error", to_string(registered.error))
            .field("decision", "reject_request");
        return map_auth_error(registered.error);
    }

    common::Logger::instance()
        .info(common::LogDomain::Auth, "register succeeded")
        .field("user_id", registered.user.user_id)
        .field("username", registered.user.username);

    common::JsonObject data;
    data.emplace("user", user_json(registered.user));
    data.emplace("access_token", common::JsonValue(registered.access_token));
    return http::make_api_success_response(common::JsonValue(std::move(data)));
}

http::HttpResponse handle_login(const std::shared_ptr<AuthService>& auth_service, const http::RouteContext& context) {
    const std::optional<std::pair<std::string, std::string>> credentials = parse_credentials(context.request.body);
    if (!credentials.has_value()) {
        common::Logger::instance()
            .warn(common::LogDomain::Auth, "login rejected")
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const LoginResult login = auth_service->login(credentials->first, credentials->second);
    if (login.error != AuthErrorCode::Ok) {
        common::Logger::instance()
            .warn(common::LogDomain::Auth, "login rejected")
            .field("username", credentials->first)
            .field("error", to_string(login.error))
            .field("decision", "reject_request");
        return map_auth_error(login.error);
    }

    common::Logger::instance()
        .info(common::LogDomain::Auth, "login succeeded")
        .field("user_id", login.user.user_id)
        .field("username", login.user.username);

    common::JsonObject data;
    data.emplace("user", user_json(login.user));
    data.emplace("access_token", common::JsonValue(login.access_token));
    return http::make_api_success_response(common::JsonValue(std::move(data)));
}

http::HttpResponse handle_me(const std::shared_ptr<AuthService>& auth_service, const http::RouteContext& context) {
    const std::string token = extract_bearer_token(context.request.headers);
    const AuthenticateResult authenticated = auth_service->authenticate_access_token(token);
    if (authenticated.error != AuthErrorCode::Ok) {
        common::Logger::instance()
            .warn(common::LogDomain::Auth, "access token verification failed")
            .field("error", to_string(authenticated.error))
            .field("decision", "reject_request");
        return map_auth_error(authenticated.error);
    }

    common::Logger::instance()
        .info(common::LogDomain::Auth, "access token verified")
        .field("user_id", authenticated.user.user_id)
        .field("username", authenticated.user.username);
    common::JsonObject data;
    data.emplace("user", user_json(authenticated.user));
    return http::make_api_success_response(common::JsonValue(std::move(data)));
}

}  // namespace

bool register_auth_routes(const server::ServerConfig& config, const std::shared_ptr<http::Router>& router) {
    if (router == nullptr) {
        return false;
    }

    const std::optional<std::string> jwt_secret = load_or_create_jwt_secret(config.auth_jwt_secret_path);
    if (!jwt_secret.has_value()) {
        return false;
    }

    const std::optional<std::string> password = resolve_database_password(config);
    if (!password.has_value()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "auth database password resolve failed")
            .field("error", "database_password_env_not_set")
            .field("password_env", config.database_password_env)
            .field("decision", "exit_process");
        return false;
    }

    const common::PostgresConnectionPool::InitializeStatus pool_status =
        common::PostgresConnectionPool::instance().initialize(common::PostgresConnectionPoolOptions{
            .host = config.database_host,
            .port = config.database_port,
            .database = config.database_name,
            .user = config.database_user,
            .password = *password,
            .max_connections = config.database_max_connections,
            .connect_timeout_ms = config.database_connect_timeout_ms,
            .acquire_timeout_ms = config.database_acquire_timeout_ms,
        });
    if (pool_status != common::PostgresConnectionPool::InitializeStatus::Initialized &&
        pool_status != common::PostgresConnectionPool::InitializeStatus::AlreadyInitialized) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "auth postgres pool init failed")
            .field("error", to_string(pool_status))
            .field("decision", "exit_process");
        return false;
    }

    if (!user::check_user_schema_ready()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "auth user schema check failed")
            .field("error", "user_schema_not_ready")
            .field("decision", "exit_process");
        return false;
    }

    auto shared_auth_service = std::make_shared<AuthService>(
        PasswordHasher({.iterations = config.auth_password_hash_iterations}),
        JwtService({.secret = *jwt_secret, .access_token_ttl_s = config.auth_access_token_ttl_s}));

    if (!register_route(router, http::HttpMethod::Post, "/api/auth/register",
                        std::bind_front(handle_register, shared_auth_service))) {
        return false;
    }

    if (!register_route(router, http::HttpMethod::Post, "/api/auth/login",
                        std::bind_front(handle_login, shared_auth_service))) {
        return false;
    }

    if (!register_route(router, http::HttpMethod::Get, "/api/auth/me",
                        std::bind_front(handle_me, shared_auth_service))) {
        return false;
    }

    return true;
}

}  // namespace nebula::auth
