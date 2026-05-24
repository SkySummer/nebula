#include "nebula/storage/http/routes.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/common/log/logger.hpp"
#include "nebula/storage/http/handlers.hpp"

namespace nebula::storage {

namespace {

bool register_route(const std::shared_ptr<http::Router>& router, http::HttpMethod method, std::string_view path,
                    http::RouteHandler handler, http::RouteOptions options = {}) {
    if (router->add_route(method, std::string(path), std::move(handler), options)) {
        return true;
    }

    common::Logger::instance()
        .fatal("register storage route failed")
        .field("method", http::to_string(method))
        .field("path", path)
        .field("error", "register_route_failed")
        .field("decision", "exit_process");
    return false;
}

}  // namespace

bool register_storage_routes(const std::shared_ptr<http::Router>& router,
                             const std::shared_ptr<StorageService>& service) {
    if (router == nullptr || service == nullptr) {
        return false;
    }

    if (!register_route(router, http::HttpMethod::Post, "/api/storage/uploads/init",
                        std::bind_front(handle_upload_init, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/storage/uploads/{upload_id}/chunks/{chunk_index}",
                        std::bind_front(handle_upload_chunk, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/storage/uploads/{upload_id}/complete",
                        std::bind_front(handle_upload_complete, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Put, "/api/storage/directories/{path_b64}",
                        std::bind_front(handle_create_directory, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/storage/files/{path_b64}/download-ticket",
                        std::bind_front(handle_issue_download_ticket, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/downloads/{download_ticket}",
                        std::bind_front(handle_download_with_ticket, service))) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/tree/{path_b64}",
                        std::bind_front(handle_tree_list, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/recent", std::bind_front(handle_recent, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Get, "/api/storage/usage", std::bind_front(handle_usage, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Delete, "/api/storage/nodes/{path_b64}",
                        std::bind_front(handle_delete_node, service), http::RouteOptions{.require_user = true})) {
        return false;
    }
    if (!register_route(router, http::HttpMethod::Post, "/api/storage/gc", std::bind_front(handle_gc, service),
                        http::RouteOptions{.require_user = true})) {
        return false;
    }

    return true;
}

}  // namespace nebula::storage
