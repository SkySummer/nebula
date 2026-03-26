#ifndef NEBULA_HTTP_ROUTER_HPP
#define NEBULA_HTTP_ROUTER_HPP

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula/http/http_types.hpp"

namespace nebula::http {

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
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    bool add_route(HttpMethod method, const std::string& path, Handler handler);
    bool mod_route(HttpMethod method, const std::string& path, Handler handler);
    bool del_route(HttpMethod method, const std::string& path);
    [[nodiscard]] RouteDispatchResult dispatch(const HttpRequest& request) const;

private:
    struct HttpMethodHash {
        std::size_t operator()(HttpMethod method) const noexcept {
            return static_cast<std::size_t>(method);
        }
    };

    using MethodMap = std::unordered_map<HttpMethod, Handler, HttpMethodHash>;
    mutable std::shared_mutex route_mutex_;
    std::unordered_map<std::string, MethodMap> route_table_;
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_ROUTER_HPP
