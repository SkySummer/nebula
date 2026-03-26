#include "nebula/http/router.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "nebula/common/logger.hpp"

namespace nebula::http {

bool Router::add_route(HttpMethod method, const std::string& path, Handler handler) {
    if (path.empty()) {
        common::Logger::instance()
            .warn("add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (!handler) {
        common::Logger::instance()
            .warn("add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_handler");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    MethodMap& method_map = route_table_[path];
    const auto [it, inserted] = method_map.emplace(method, std::move(handler));
    if (!inserted) {
        common::Logger::instance()
            .warn("add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_route");
        return false;
    }

    common::Logger::instance().info("route added").field("method", to_string(method)).field("path", path);
    return inserted;
}

bool Router::mod_route(HttpMethod method, const std::string& path, Handler handler) {
    if (path.empty()) {
        common::Logger::instance()
            .warn("mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (!handler) {
        common::Logger::instance()
            .warn("mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_handler");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    const auto path_it = route_table_.find(path);
    if (path_it == route_table_.end()) {
        common::Logger::instance()
            .warn("mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "path_not_found");
        return false;
    }

    MethodMap& method_map = path_it->second;
    const auto method_it = method_map.find(method);
    if (method_it == method_map.end()) {
        common::Logger::instance()
            .warn("mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "method_not_found");
        return false;
    }

    method_it->second = std::move(handler);

    common::Logger::instance().info("route modified").field("method", to_string(method)).field("path", path);
    return true;
}

bool Router::del_route(HttpMethod method, const std::string& path) {
    if (path.empty()) {
        common::Logger::instance()
            .warn("del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    const auto path_it = route_table_.find(path);
    if (path_it == route_table_.end()) {
        common::Logger::instance()
            .warn("del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "path_not_found");
        return false;
    }

    MethodMap& method_map = path_it->second;
    const std::size_t erased = method_map.erase(method);
    if (erased == 0U) {
        common::Logger::instance()
            .warn("del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "method_not_found");
        return false;
    }

    if (method_map.empty()) {
        route_table_.erase(path_it);
    }

    common::Logger::instance().info("route deleted").field("method", to_string(method)).field("path", path);
    return true;
}

RouteDispatchResult Router::dispatch(const HttpRequest& request) const {
    Handler handler;
    std::vector<HttpMethod> allowed_methods;
    {
        const std::shared_lock lock(route_mutex_);
        const auto path_it = route_table_.find(request.path);
        if (path_it == route_table_.end()) {
            return {.status = RouteStatus::NotFound, .response = {}, .allowed_methods = {}};
        }

        const MethodMap& method_map = path_it->second;
        const auto method_it = method_map.find(request.method);
        if (method_it == method_map.end()) {
            allowed_methods.reserve(method_map.size());
            for (const auto& route_entry : method_map) {
                allowed_methods.push_back(route_entry.first);
            }
            std::ranges::sort(allowed_methods, [](HttpMethod lhs, HttpMethod rhs) { return lhs < rhs; });
            return {
                .status = RouteStatus::MethodNotAllowed, .response = {}, .allowed_methods = std::move(allowed_methods)};
        }

        handler = method_it->second;
    }

    return {.status = RouteStatus::Matched, .response = handler(request), .allowed_methods = {}};
}

}  // namespace nebula::http
