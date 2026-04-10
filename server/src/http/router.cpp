#include "nebula/http/router.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "nebula/common/logger.hpp"

namespace nebula::http {

namespace {

struct RouteSegment {
    bool dynamic = false;
    std::string value;
};

bool has_mixed_dynamic_marker(std::string_view segment) {
    if (segment.empty()) {
        return false;
    }

    const bool has_open_brace = segment.find('{') != std::string_view::npos;
    const bool has_close_brace = segment.find('}') != std::string_view::npos;
    if (!has_open_brace && !has_close_brace) {
        return false;
    }

    if (segment.size() < 3U || segment.front() != '{' || segment.back() != '}') {
        return true;
    }

    const std::string_view param_name = segment.substr(1U, segment.size() - 2U);
    if (param_name.empty()) {
        return true;
    }

    return param_name.find('{') != std::string_view::npos || param_name.find('}') != std::string_view::npos;
}

bool parse_route_segments(std::string_view path, std::vector<RouteSegment>& out_segments) {
    out_segments.clear();
    const std::vector<std::string> split_segments = split_http_path_segments(path);
    out_segments.reserve(split_segments.size());

    for (const std::string& segment : split_segments) {
        if (has_mixed_dynamic_marker(segment)) {
            return false;
        }

        const bool is_dynamic = segment.size() >= 3U && segment.front() == '{' && segment.back() == '}';

        out_segments.push_back(RouteSegment{
            .dynamic = is_dynamic,
            .value = is_dynamic ? segment.substr(1U, segment.size() - 2U) : segment,
        });
    }
    return true;
}

std::vector<std::string> collect_dynamic_param_keys(const std::vector<RouteSegment>& segments) {
    std::vector<std::string> keys;
    keys.reserve(segments.size());
    for (const RouteSegment& segment : segments) {
        if (!segment.dynamic) {
            continue;
        }
        keys.push_back(segment.value);
    }
    std::ranges::sort(keys);
    const auto deduplicated = std::ranges::unique(keys);
    keys.erase(deduplicated.begin(), deduplicated.end());
    return keys;
}

bool has_same_dynamic_param_keys(const std::vector<RouteSegment>& lhs_segments,
                                 const std::vector<RouteSegment>& rhs_segments) {
    return collect_dynamic_param_keys(lhs_segments) == collect_dynamic_param_keys(rhs_segments);
}

bool has_duplicate_dynamic_param_keys(const std::vector<RouteSegment>& segments) {
    std::size_t dynamic_param_count = 0U;
    for (const RouteSegment& segment : segments) {
        if (segment.dynamic) {
            ++dynamic_param_count;
        }
    }
    return collect_dynamic_param_keys(segments).size() != dynamic_param_count;
}

bool node_is_empty(const Router::RouteNode& node) {
    return node.handlers.empty() && node.static_children.empty() && !node.dynamic_child.has_value();
}

Router::RouteNode* find_exact_route_node(Router::RouteNode& root, const std::vector<RouteSegment>& segments) {
    Router::RouteNode* current = &root;
    for (const RouteSegment& segment : segments) {
        if (!segment.dynamic) {
            const auto child_it = current->static_children.find(segment.value);
            if (child_it == current->static_children.end()) {
                return nullptr;
            }
            current = child_it->second.get();
            continue;
        }

        if (!current->dynamic_child.has_value() || current->dynamic_child->param_name != segment.value) {
            return nullptr;
        }
        current = current->dynamic_child->node.get();
    }
    return current;
}

const Router::RouteNode* find_match_route_node(const Router::RouteNode& node, const std::vector<std::string>& segments,
                                               std::size_t depth, RouteParams& params) {
    if (depth == segments.size()) {
        return node.handlers.empty() ? nullptr : &node;
    }

    const std::string& segment = segments[depth];
    const auto static_child_it = node.static_children.find(segment);
    if (static_child_it != node.static_children.end()) {
        RouteParams static_params = params;
        if (const Router::RouteNode* matched =
                find_match_route_node(*static_child_it->second, segments, depth + 1U, static_params);
            matched != nullptr) {
            params = std::move(static_params);
            return matched;
        }
    }

    if (!node.dynamic_child.has_value() || segment.empty()) {
        return nullptr;
    }

    RouteParams dynamic_params = params;
    dynamic_params[node.dynamic_child->param_name] = segment;
    if (const Router::RouteNode* matched =
            find_match_route_node(*node.dynamic_child->node, segments, depth + 1U, dynamic_params);
        matched != nullptr) {
        params = std::move(dynamic_params);
        return matched;
    }
    return nullptr;
}

void collect_allowed_methods(const Router::MethodMap& method_map, std::vector<HttpMethod>& allowed_methods) {
    allowed_methods.clear();
    allowed_methods.reserve(method_map.size());
    for (const auto& route_entry : method_map) {
        allowed_methods.push_back(route_entry.first);
    }
    std::ranges::sort(allowed_methods, [](HttpMethod lhs, HttpMethod rhs) { return lhs < rhs; });
}

bool erase_route_recursive(Router::RouteNode& node, const std::vector<RouteSegment>& segments, std::size_t depth,
                           HttpMethod method) {
    if (depth == segments.size()) {
        node.handlers.erase(method);
        return node_is_empty(node);
    }

    const RouteSegment& segment = segments[depth];
    if (!segment.dynamic) {
        const auto child_it = node.static_children.find(segment.value);
        if (child_it == node.static_children.end()) {
            return node_is_empty(node);
        }

        if (erase_route_recursive(*child_it->second, segments, depth + 1U, method)) {
            node.static_children.erase(child_it);
        }
        return node_is_empty(node);
    }

    if (!node.dynamic_child.has_value() || node.dynamic_child->param_name != segment.value) {
        return node_is_empty(node);
    }

    if (erase_route_recursive(*node.dynamic_child->node, segments, depth + 1U, method)) {
        node.dynamic_child.reset();
    }
    return node_is_empty(node);
}

const Router::RouteNode* find_matched_route_node(const Router::RouteNode& root,
                                                 const std::vector<std::string>& segments, RouteParams& params) {
    params.clear();
    return find_match_route_node(root, segments, 0U, params);
}

bool add_route_with_handler_locked(Router::RouteNode& root,
                                   std::unordered_map<std::string, Router::ExactMethodSet>& exact_route_index,
                                   HttpMethod method, const std::string& path,
                                   const std::vector<RouteSegment>& segments, Router::HandlerPtr handler) {
    Router::RouteNode* current = &root;
    for (const RouteSegment& segment : segments) {
        if (!segment.dynamic) {
            auto& child = current->static_children[segment.value];
            if (!child) {
                child = std::make_unique<Router::RouteNode>();
            }
            current = child.get();
            continue;
        }

        if (!current->dynamic_child.has_value()) {
            current->dynamic_child = Router::DynamicChild{
                .param_name = segment.value,
                .node = std::make_unique<Router::RouteNode>(),
            };
        } else if (current->dynamic_child->param_name != segment.value) {
            common::Logger::instance()
                .warn(common::LogDomain::Http, "add route rejected")
                .field("method", to_string(method))
                .field("path", path)
                .field("error", "ambiguous_dynamic_segment");
            return false;
        }
        current = current->dynamic_child->node.get();
    }

    const auto [handler_it, inserted] = current->handlers.emplace(method, std::move(handler));
    if (!inserted) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_route");
        return false;
    }

    exact_route_index[path][method] = true;

    common::Logger::instance()
        .info(common::LogDomain::Http, "route added")
        .field("method", to_string(method))
        .field("path", path);
    return true;
}

bool mod_route_with_handler_locked(Router::RouteNode& root,
                                   const std::unordered_map<std::string, Router::ExactMethodSet>& exact_route_index,
                                   HttpMethod method, const std::string& path,
                                   const std::vector<RouteSegment>& segments, Router::HandlerPtr handler) {
    const auto exact_path_it = exact_route_index.find(path);
    if (exact_path_it == exact_route_index.end()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "path_not_found");
        return false;
    }

    if (!exact_path_it->second.contains(method)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "method_not_found");
        return false;
    }

    Router::RouteNode* route_node = find_exact_route_node(root, segments);
    if (route_node == nullptr) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "path_not_found");
        return false;
    }

    auto method_it = route_node->handlers.find(method);
    if (method_it == route_node->handlers.end()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "method_not_found");
        return false;
    }

    method_it->second = std::move(handler);
    common::Logger::instance()
        .info(common::LogDomain::Http, "route modified")
        .field("method", to_string(method))
        .field("path", path);
    return true;
}

}  // namespace

bool Router::add_route(HttpMethod method, const std::string& path, Handler handler) {
    if (path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (!handler) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_handler");
        return false;
    }

    std::vector<RouteSegment> segments;
    if (!parse_route_segments(path, segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "invalid_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_dynamic_param");
        return false;
    }

    auto handler_ptr = std::make_shared<Handler>(std::move(handler));
    const std::unique_lock lock(route_mutex_);
    return add_route_with_handler_locked(root_, exact_route_index_, method, path, segments, std::move(handler_ptr));
}

bool Router::add_route(HttpMethod method, const std::string& path, const std::string& source_path) {
    if (path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (source_path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "empty_source_path");
        return false;
    }

    std::vector<RouteSegment> segments;
    if (!parse_route_segments(path, segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "invalid_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_dynamic_param");
        return false;
    }

    std::vector<RouteSegment> source_segments;
    if (!parse_route_segments(source_path, source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "invalid_source_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "duplicate_source_dynamic_param");
        return false;
    }
    if (!has_same_dynamic_param_keys(segments, source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_dynamic_keys_mismatch");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    const auto source_path_it = exact_route_index_.find(source_path);
    if (source_path_it == exact_route_index_.end()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_path_not_found");
        return false;
    }

    if (!source_path_it->second.contains(method)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_method_not_found");
        return false;
    }

    const RouteNode* source_node = find_exact_route_node(root_, source_segments);
    if (source_node == nullptr) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_path_not_found");
        return false;
    }

    const auto source_method_it = source_node->handlers.find(method);
    if (source_method_it == source_node->handlers.end() || source_method_it->second == nullptr) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "add route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_method_not_found");
        return false;
    }

    return add_route_with_handler_locked(root_, exact_route_index_, method, path, segments, source_method_it->second);
}

bool Router::mod_route(HttpMethod method, const std::string& path, Handler handler) {
    if (path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (!handler) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_handler");
        return false;
    }

    std::vector<RouteSegment> segments;
    if (!parse_route_segments(path, segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "invalid_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_dynamic_param");
        return false;
    }

    auto handler_ptr = std::make_shared<Handler>(std::move(handler));
    const std::unique_lock lock(route_mutex_);
    return mod_route_with_handler_locked(root_, exact_route_index_, method, path, segments, std::move(handler_ptr));
}

bool Router::mod_route(HttpMethod method, const std::string& path, const std::string& source_path) {
    if (path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }
    if (source_path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "empty_source_path");
        return false;
    }

    std::vector<RouteSegment> segments;
    if (!parse_route_segments(path, segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "invalid_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "duplicate_dynamic_param");
        return false;
    }

    std::vector<RouteSegment> source_segments;
    if (!parse_route_segments(source_path, source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "invalid_source_dynamic_segment");
        return false;
    }
    if (has_duplicate_dynamic_param_keys(source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "duplicate_source_dynamic_param");
        return false;
    }
    if (!has_same_dynamic_param_keys(segments, source_segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_dynamic_keys_mismatch");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    const auto source_path_it = exact_route_index_.find(source_path);
    if (source_path_it == exact_route_index_.end()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_path_not_found");
        return false;
    }

    if (!source_path_it->second.contains(method)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_method_not_found");
        return false;
    }

    const RouteNode* source_node = find_exact_route_node(root_, source_segments);
    if (source_node == nullptr) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_path_not_found");
        return false;
    }

    const auto source_method_it = source_node->handlers.find(method);
    if (source_method_it == source_node->handlers.end() || source_method_it->second == nullptr) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "mod route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("source_path", source_path)
            .field("error", "source_method_not_found");
        return false;
    }

    return mod_route_with_handler_locked(root_, exact_route_index_, method, path, segments, source_method_it->second);
}

bool Router::del_route(HttpMethod method, const std::string& path) {
    if (path.empty()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "empty_path");
        return false;
    }

    std::vector<RouteSegment> segments;
    if (!parse_route_segments(path, segments)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "invalid_dynamic_segment");
        return false;
    }

    const std::unique_lock lock(route_mutex_);
    const auto exact_path_it = exact_route_index_.find(path);
    if (exact_path_it == exact_route_index_.end()) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "path_not_found");
        return false;
    }

    if (!exact_path_it->second.contains(method)) {
        common::Logger::instance()
            .warn(common::LogDomain::Http, "del route rejected")
            .field("method", to_string(method))
            .field("path", path)
            .field("error", "method_not_found");
        return false;
    }

    erase_route_recursive(root_, segments, 0U, method);

    exact_path_it->second.erase(method);
    if (exact_path_it->second.empty()) {
        exact_route_index_.erase(exact_path_it);
    }

    common::Logger::instance()
        .info(common::LogDomain::Http, "route deleted")
        .field("method", to_string(method))
        .field("path", path);
    return true;
}

bool Router::has_route_match(HttpMethod method, const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    const std::vector<std::string> segments = split_http_path_segments(path);
    RouteParams params;
    const std::shared_lock lock(route_mutex_);
    const RouteNode* route_node = find_matched_route_node(root_, segments, params);
    if (route_node == nullptr) {
        return false;
    }
    return route_node->handlers.contains(method);
}

bool Router::has_route_exact(HttpMethod method, const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    const std::shared_lock lock(route_mutex_);
    const auto path_it = exact_route_index_.find(path);
    if (path_it == exact_route_index_.end()) {
        return false;
    }
    return path_it->second.contains(method);
}

RouteDispatchResult Router::dispatch(HttpRequest request) const {
    HandlerPtr handler;
    RouteParams params;
    std::vector<HttpMethod> allowed_methods;
    {
        const std::vector<std::string> segments = split_http_path_segments(request.path);

        const std::shared_lock lock(route_mutex_);
        const RouteNode* route_node = find_matched_route_node(root_, segments, params);
        if (route_node == nullptr) {
            return {.status = RouteStatus::NotFound, .response = {}, .allowed_methods = {}};
        }

        const auto method_it = route_node->handlers.find(request.method);
        if (method_it == route_node->handlers.end()) {
            collect_allowed_methods(route_node->handlers, allowed_methods);
            return {
                .status = RouteStatus::MethodNotAllowed,
                .response = {},
                .allowed_methods = std::move(allowed_methods),
            };
        }

        handler = method_it->second;
    }

    return {.status = RouteStatus::Matched,
            .response = (*handler)(RouteContext{.request = std::move(request), .params = std::move(params)}),
            .allowed_methods = {}};
}

}  // namespace nebula::http
