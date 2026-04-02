#include "nebula/server/server_app.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "nebula/common/logger.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/http_server.hpp"

namespace nebula::server {

namespace {

bool path_contains_route_template_marker(std::string_view path) {
    return path.find('{') != std::string_view::npos || path.find('}') != std::string_view::npos;
}

std::shared_ptr<http::Router> build_default_router(const ServerConfig& config) {
    auto router = std::make_shared<http::Router>();

    if (config.enable_healthz) {
        const bool added = router->add_route(http::HttpMethod::Get, "/healthz", [](const http::RouteContext&) {
            http::HttpResponse response;
            response.status = http::HttpStatus::OK;
            response.headers.emplace("Content-Type", "application/json");
            response.body = R"({"status":"ok"})";
            return response;
        });
        if (!added) {
            common::Logger::instance()
                .error("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Get))
                .field("path", "/healthz")
                .field("decision", "exit_process");
            return nullptr;
        }
    }

    if (config.enable_echo) {
        const bool added = router->add_route(http::HttpMethod::Post, "/echo", [](const http::RouteContext& context) {
            http::HttpResponse response;
            response.status = http::HttpStatus::OK;
            response.headers.emplace("Content-Type", "application/json");
            response.body = context.request.body;
            return response;
        });
        if (!added) {
            common::Logger::instance()
                .error("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Post))
                .field("path", "/echo")
                .field("decision", "exit_process");
            return nullptr;
        }
    }

    if (!config.enable_root_default) {
        return router;
    }

    const std::string root_default_path = config.root_default_path;
    if (path_contains_route_template_marker(root_default_path)) {
        common::Logger::instance()
            .error("register root route mapping failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("target_path", root_default_path)
            .field("error", "path_template_not_allowed")
            .field("decision", "exit_process");
        return nullptr;
    }
    if (!router->has_route_match(http::HttpMethod::Get, root_default_path)) {
        common::Logger::instance()
            .error("register root route mapping failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("target_path", root_default_path)
            .field("error", "path_not_found")
            .field("decision", "exit_process");
        return nullptr;
    }

    const std::weak_ptr<http::Router> router_ref = router;
    const bool root_added = router->add_route(
        http::HttpMethod::Get, "/", [router_ref, root_default_path](const http::RouteContext& context) {
            const std::shared_ptr<http::Router> mapped_router = router_ref.lock();
            if (mapped_router == nullptr) {
                return http::make_error_response(http::HttpStatus::NotFound);
            }

            http::HttpRequest mapped_request = context.request;
            mapped_request.path = root_default_path;

            const http::RouteDispatchResult mapped = mapped_router->dispatch(std::move(mapped_request));
            switch (mapped.status) {
                case http::RouteStatus::Matched:
                    return mapped.response;
                case http::RouteStatus::MethodNotAllowed:
                    return http::make_error_response(http::HttpStatus::MethodNotAllowed);
                case http::RouteStatus::NotFound:
                    return http::make_error_response(http::HttpStatus::NotFound);
            }
            return http::make_error_response(http::HttpStatus::NotFound);
        });
    if (!root_added) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("decision", "exit_process");
        return nullptr;
    }

    return router;
}

}  // namespace

ServerApp::ServerApp(std::span<char*> args) : startup_(args) {}

int ServerApp::run() const {
    if (!startup_.ok) {
        std::cerr << startup_.error << '\n';
        return 1;
    }

    common::Logger::instance().init(startup_.config.log_level, startup_.config.log_dir,
                                    startup_.config.log_also_stderr);

    if (startup_.config_source == ServerConfigSource::Default) {
        common::Logger::instance()
            .info("server config defaults applied")
            .field("source", to_string(startup_.config_source));
    } else {
        common::Logger::instance()
            .info("server config loaded")
            .field("source", to_string(startup_.config_source))
            .field("path", startup_.config_path.string());
    }

    const std::shared_ptr<http::Router> router = build_default_router(startup_.config);
    if (router == nullptr) {
        return 1;
    }

    HttpServerRuntime runtime = HttpServerBuilder().with_config(startup_.config).with_router(router).build();
    const RunResult run_result = runtime.run();
    const bool success = is_successful_run_result(run_result);
    const int exit_code = success ? 0 : 1;

    common::Logger::instance()
        .info("server process exiting")
        .field("run_result", to_string(run_result))
        .field("exit_code", exit_code)
        .field("result", success ? "success" : "failed");

    return exit_code;
}

}  // namespace nebula::server
