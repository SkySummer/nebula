#include "nebula/auth/http/handlers.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/auth/http/responses.hpp"
#include "nebula/common/base/string.hpp"
#include "nebula/common/codec/json.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/http/redaction/request_redaction.hpp"
#include "nebula/http/codec/response_writer.hpp"

namespace nebula::auth {

namespace {

constexpr std::int64_t kDefaultUserListLimit = 50;
constexpr std::int64_t kMaxUserListLimit = 200;

struct PasswordChangeRequest {
    std::string current_password;
    std::string new_password;
};

struct UserListQuery {
    std::int64_t limit = kDefaultUserListLimit;
    std::int64_t offset = 0;
};

struct UserUpdateRequest {
    std::optional<UserRole> role;
    std::optional<UserStatus> status;
};

common::JsonValue build_user_json(const UserProfile& user) {
    common::JsonObject object;
    object.emplace("user_id", user.user_id);
    object.emplace("username", user.username);
    object.emplace("role", to_string(user.role));
    object.emplace("status", to_string(user.status));
    object.emplace("created_at_s", user.created_at_s);
    return common::JsonValue(std::move(object));
}

bool json_get_required_string(const common::JsonObject& object, std::string_view key, std::string& value) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return false;
    }
    const std::string* raw = it->second.get_if_string();
    if (raw == nullptr) {
        return false;
    }
    value = *raw;
    return true;
}

std::optional<common::JsonObject> parse_json_object(std::string_view body) {
    const common::JsonParseResult parsed = common::parse_json(body);
    if (!parsed.ok) {
        return std::nullopt;
    }
    const common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }
    return *object;
}

std::optional<std::pair<std::string, std::string>> parse_credentials(std::string_view body) {
    const std::optional<common::JsonObject> object = parse_json_object(body);
    if (!object.has_value()) {
        return std::nullopt;
    }

    std::string username;
    std::string password;
    if (!json_get_required_string(*object, "username", username) ||
        !json_get_required_string(*object, "password", password)) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::move(username), std::move(password)};
}

std::optional<PasswordChangeRequest> parse_password_change_request(std::string_view body) {
    const std::optional<common::JsonObject> object = parse_json_object(body);
    if (!object.has_value()) {
        return std::nullopt;
    }

    PasswordChangeRequest request;
    if (!json_get_required_string(*object, "current_password", request.current_password) ||
        !json_get_required_string(*object, "new_password", request.new_password)) {
        return std::nullopt;
    }
    return request;
}

std::optional<UserUpdateRequest> parse_user_update_request(std::string_view body) {
    const std::optional<common::JsonObject> object = parse_json_object(body);
    if (!object.has_value()) {
        return std::nullopt;
    }

    UserUpdateRequest request;
    for (const auto& [key, value] : *object) {
        const std::string* raw = value.get_if_string();
        if (raw == nullptr) {
            return std::nullopt;
        }
        if (key == "role") {
            request.role = parse_user_role(*raw);
            if (!request.role.has_value()) {
                return std::nullopt;
            }
            continue;
        }
        if (key == "status") {
            request.status = parse_user_status(*raw);
            if (!request.status.has_value()) {
                return std::nullopt;
            }
            continue;
        }
        return std::nullopt;
    }

    if (!request.role.has_value() && !request.status.has_value()) {
        return std::nullopt;
    }
    return request;
}

std::optional<std::int64_t> parse_route_user_id(const http::RouteContext& context) {
    const auto id_it = context.params.find("user_id");
    if (id_it == context.params.end()) {
        return std::nullopt;
    }
    const auto user_id = common::parse_number<std::int64_t>(id_it->second);
    if (!user_id.has_value() || *user_id <= 0) {
        return std::nullopt;
    }
    return *user_id;
}

std::optional<UserListQuery> parse_user_list_query(const http::QueryParams& query_params) {
    UserListQuery query;

    const auto limit_it = query_params.find("limit");
    if (limit_it != query_params.end()) {
        if (limit_it->second.size() != 1U) {
            return std::nullopt;
        }
        const auto limit = common::parse_number<std::int64_t>(limit_it->second.front());
        if (!limit.has_value() || *limit <= 0 || *limit > kMaxUserListLimit) {
            return std::nullopt;
        }
        query.limit = *limit;
    }

    const auto offset_it = query_params.find("offset");
    if (offset_it != query_params.end()) {
        if (offset_it->second.size() != 1U) {
            return std::nullopt;
        }
        const auto offset = common::parse_number<std::int64_t>(offset_it->second.front());
        if (!offset.has_value() || *offset < 0) {
            return std::nullopt;
        }
        query.offset = *offset;
    }

    return query;
}

std::optional<UserProfile> require_authenticated_user(const http::RouteContext& context) {
    if (!context.user.has_value()) {
        common::Logger::instance()
            .error("authenticated route missing user context")
            .field("path", nebula::http::redact_request_path(context.request.path))
            .field("error", "missing_authenticated_user")
            .field("decision", "return_internal_error");
        return std::nullopt;
    }
    return context.user;
}

}  // namespace

http::HttpResponse handle_register(const std::shared_ptr<AuthService>& auth_service,
                                   const http::RouteContext& context) {
    const std::optional<std::pair<std::string, std::string>> credentials = parse_credentials(context.request.body);
    if (!credentials.has_value()) {
        common::Logger::instance()
            .warn("register rejected")
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const auto registered = auth_service->register_user(credentials->first, credentials->second);
    if (!registered.has_value()) {
        common::Logger::instance()
            .warn("register rejected")
            .field("username", credentials->first)
            .field("error", to_string(registered.error()))
            .field("decision", "reject_request");
        return to_http_response(registered.error());
    }

    common::Logger::instance()
        .info("register succeeded")
        .field("user_id", registered->user.user_id)
        .field("username", registered->user.username)
        .field("role", to_string(registered->user.role));

    common::JsonObject data;
    data.emplace("user", build_user_json(registered->user));
    data.emplace("access_token", registered->access_token);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_login(const std::shared_ptr<AuthService>& auth_service, const http::RouteContext& context) {
    const std::optional<std::pair<std::string, std::string>> credentials = parse_credentials(context.request.body);
    if (!credentials.has_value()) {
        common::Logger::instance()
            .warn("login rejected")
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const auto login = auth_service->login_user(credentials->first, credentials->second);
    if (!login.has_value()) {
        common::Logger::instance()
            .warn("login rejected")
            .field("username", credentials->first)
            .field("error", to_string(login.error()))
            .field("decision", "reject_request");
        return to_http_response(login.error());
    }

    common::Logger::instance()
        .info("login succeeded")
        .field("user_id", login->user.user_id)
        .field("username", login->user.username);

    common::JsonObject data;
    data.emplace("user", build_user_json(login->user));
    data.emplace("access_token", login->access_token);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_me(const std::shared_ptr<AuthService>& auth_service, const http::RouteContext& context) {
    if (auth_service == nullptr) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<UserProfile> user = require_authenticated_user(context);
    if (!user.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    common::Logger::instance()
        .info("access token verified")
        .field("user_id", user->user_id)
        .field("username", user->username);

    common::JsonObject data;
    data.emplace("user", build_user_json(*user));
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_change_password(const std::shared_ptr<AuthService>& auth_service,
                                          const http::RouteContext& context) {
    const std::optional<UserProfile> actor = require_authenticated_user(context);
    if (!actor.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<PasswordChangeRequest> request = parse_password_change_request(context.request.body);
    if (!request.has_value()) {
        common::Logger::instance()
            .warn("change password rejected")
            .field("user_id", actor->user_id)
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const auto changed = auth_service->change_password(*actor, request->current_password, request->new_password);
    if (!changed.has_value()) {
        common::Logger::instance()
            .warn("change password rejected")
            .field("user_id", actor->user_id)
            .field("error", to_string(changed.error()))
            .field("decision", "reject_request");
        return to_http_response(changed.error());
    }

    common::Logger::instance()
        .info("password changed")
        .field("user_id", changed->user.user_id)
        .field("username", changed->user.username);

    common::JsonObject data;
    data.emplace("user", build_user_json(changed->user));
    data.emplace("access_token", changed->access_token);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_list_users(const std::shared_ptr<AuthService>& auth_service,
                                     const http::RouteContext& context) {
    const std::optional<UserProfile> actor = require_authenticated_user(context);
    if (!actor.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<UserListQuery> query = parse_user_list_query(context.request.query_params);
    if (!query.has_value()) {
        common::Logger::instance()
            .warn("list users rejected")
            .field("user_id", actor->user_id)
            .field("error", "invalid_query_params")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid query params");
    }

    const auto listed = auth_service->list_users(*actor, query->limit, query->offset);
    if (!listed.has_value()) {
        common::Logger::instance()
            .warn("list users rejected")
            .field("user_id", actor->user_id)
            .field("error", to_string(listed.error()))
            .field("decision", "reject_request");
        return to_http_response(listed.error());
    }

    common::JsonArray users_json;
    users_json.reserve(listed->users.size());
    for (const UserProfile& user : listed->users) {
        users_json.push_back(build_user_json(user));
    }

    common::JsonObject data;
    data.emplace("users", common::JsonValue(std::move(users_json)));
    data.emplace("limit", listed->limit);
    data.emplace("offset", listed->offset);
    data.emplace("has_more", listed->has_more);
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_get_user(const std::shared_ptr<AuthService>& auth_service,
                                   const http::RouteContext& context) {
    const std::optional<UserProfile> actor = require_authenticated_user(context);
    if (!actor.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<std::int64_t> user_id = parse_route_user_id(context);
    if (!user_id.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid user id");
    }

    const auto found = auth_service->get_user(*actor, *user_id);
    if (!found.has_value()) {
        common::Logger::instance()
            .warn("get user rejected")
            .field("actor_user_id", actor->user_id)
            .field("target_user_id", *user_id)
            .field("error", to_string(found.error()))
            .field("decision", "reject_request");
        return to_http_response(found.error());
    }

    common::JsonObject data;
    data.emplace("user", build_user_json(*found));
    return http::make_api_success_response(std::move(data));
}

http::HttpResponse handle_update_user(const std::shared_ptr<AuthService>& auth_service,
                                      const http::RouteContext& context) {
    const std::optional<UserProfile> actor = require_authenticated_user(context);
    if (!actor.has_value()) {
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    const std::optional<std::int64_t> user_id = parse_route_user_id(context);
    if (!user_id.has_value()) {
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid user id");
    }

    const std::optional<UserUpdateRequest> request = parse_user_update_request(context.request.body);
    if (!request.has_value()) {
        common::Logger::instance()
            .warn("update user rejected")
            .field("actor_user_id", actor->user_id)
            .field("target_user_id", *user_id)
            .field("error", "invalid_request_body")
            .field("decision", "reject_request");
        return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    }

    const auto updated = auth_service->update_user(*actor, *user_id, request->role, request->status);
    if (!updated.has_value()) {
        common::Logger::instance()
            .warn("update user rejected")
            .field("actor_user_id", actor->user_id)
            .field("target_user_id", *user_id)
            .field("error", to_string(updated.error()))
            .field("decision", "reject_request");
        return to_http_response(updated.error());
    }

    common::Logger::instance()
        .info("user updated")
        .field("actor_user_id", actor->user_id)
        .field("target_user_id", updated->user_id)
        .field("role", to_string(updated->role))
        .field("status", to_string(updated->status));

    common::JsonObject data;
    data.emplace("user", build_user_json(*updated));
    return http::make_api_success_response(std::move(data));
}

}  // namespace nebula::auth
