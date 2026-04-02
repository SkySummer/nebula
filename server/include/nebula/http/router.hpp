#ifndef NEBULA_HTTP_ROUTER_HPP
#define NEBULA_HTTP_ROUTER_HPP

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

struct RouteContext {
    HttpRequest request;
    RouteParams params;
};

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

class Router {
public:
    using Handler = std::function<HttpResponse(const RouteContext&)>;

    bool add_route(HttpMethod method, const std::string& path, Handler handler);
    bool mod_route(HttpMethod method, const std::string& path, Handler handler);
    bool del_route(HttpMethod method, const std::string& path);
    [[nodiscard]] bool has_route_match(HttpMethod method, const std::string& path) const;
    [[nodiscard]] bool has_route_exact(HttpMethod method, const std::string& path) const;
    [[nodiscard]] RouteDispatchResult dispatch(HttpRequest request) const;

    struct HttpMethodHash {
        std::size_t operator()(HttpMethod method) const noexcept {
            return static_cast<std::size_t>(method);
        }
    };

    using MethodMap = std::unordered_map<HttpMethod, Handler, HttpMethodHash>;
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
    };

private:
    mutable std::shared_mutex route_mutex_;
    RouteNode root_;
    std::unordered_map<std::string, ExactMethodSet> exact_route_index_;
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_ROUTER_HPP
