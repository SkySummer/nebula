#ifndef NEBULA_HTTP_ROUTER_HPP
#define NEBULA_HTTP_ROUTER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula/http/http_types.hpp"

namespace nebula::http {

using RouteParams = std::unordered_map<std::string, std::string>;

struct RouteOptions {
    bool require_user = false;
};

struct AuthenticatedUser {
    std::int64_t user_id = 0;
    std::string username;
    std::int64_t created_at_s = 0;
};

struct RouteContext {
    HttpRequest request;
    RouteParams params;
    std::optional<AuthenticatedUser> user;
};

using RouteHandler = std::function<HttpResponse(const RouteContext&)>;

enum class RouteStatus : std::uint8_t {
    Matched,
    NotFound,
    MethodNotAllowed,
};

struct RouteDispatchResult {
    RouteStatus status = RouteStatus::NotFound;
    HttpResponse response;
    std::vector<HttpMethod> allowed_methods;
};

struct RouteResolveResult {
    RouteStatus status = RouteStatus::NotFound;
    RouteContext context;
    RouteOptions options;
    std::vector<HttpMethod> allowed_methods;
    std::shared_ptr<RouteHandler> handler;
};

struct RouteHandlerEntry {
    std::shared_ptr<RouteHandler> handler;
    RouteOptions options;
};

class Router {
public:
    bool add_route(HttpMethod method, const std::string& path, RouteHandler handler, RouteOptions options = {});
    bool add_route(HttpMethod method, const std::string& path, const std::string& source_path);
    bool mod_route(HttpMethod method, const std::string& path, RouteHandler handler, RouteOptions options = {});
    bool mod_route(HttpMethod method, const std::string& path, const std::string& source_path);
    bool del_route(HttpMethod method, const std::string& path);
    [[nodiscard]] bool has_route_match(HttpMethod method, const std::string& path) const;
    [[nodiscard]] bool has_route_exact(HttpMethod method, const std::string& path) const;
    [[nodiscard]] std::size_t require_user_route_count() const;
    [[nodiscard]] RouteResolveResult resolve(HttpRequest request) const;
    [[nodiscard]] RouteDispatchResult dispatch(HttpRequest request) const;

    struct HttpMethodHash {
        std::size_t operator()(HttpMethod method) const noexcept {
            return static_cast<std::size_t>(method);
        }
    };

    using MethodMap = std::unordered_map<HttpMethod, RouteHandlerEntry, HttpMethodHash>;
    using ExactMethodSet = std::unordered_map<HttpMethod, bool, HttpMethodHash>;

    struct RouteNode;

    struct DynamicChild {
        std::string param_name;
        std::unique_ptr<RouteNode> node;
    };

    struct RouteNode {
        MethodMap handlers;
        std::unordered_map<std::string, std::unique_ptr<RouteNode>> static_children;
        std::optional<DynamicChild> dynamic_child;

        [[nodiscard]] bool empty() const;
    };

private:
    bool add_route_with_handler_locked(HttpMethod method, const std::string& path, RouteHandlerEntry handler_entry);
    bool mod_route_with_handler_locked(HttpMethod method, const std::string& path, RouteHandlerEntry handler_entry);

    mutable std::shared_mutex route_mutex_;
    RouteNode root_;
    std::unordered_map<std::string, ExactMethodSet> exact_route_index_;
    std::size_t require_user_route_count_ = 0U;
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_ROUTER_HPP
