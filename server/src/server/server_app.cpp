#include "nebula/server/server_app.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "nebula/common/logger.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/http_server.hpp"

namespace nebula::server {

namespace {

void report_log_emit_error(const char* event) noexcept {
    std::fputs("server app log emit failed: event=", stderr);
    std::fputs(event != nullptr ? event : "unknown", stderr);
    std::fputs(", error=logger_emit_failed, decision=ignore", stderr);
    std::fputc('\n', stderr);
}

void log_route_operation_error(const char* event, const char* fallback_event, http::HttpMethod method,
                               const std::string& path, const char* error,
                               const std::string* source_path = nullptr) noexcept {
    try {
        auto entry = common::Logger::instance().error(event != nullptr ? event : "route operation failed");
        entry.field("method", http::to_string(method)).field("path", path);
        if (source_path != nullptr) {
            entry.field("source_path", *source_path);
        }
        entry.field("error", error != nullptr ? error : "unknown");
    } catch (...) {
        report_log_emit_error(fallback_event);
    }
}

template <typename Op>
bool execute_route_operation(const char* event, const char* fallback_event, http::HttpMethod method,
                             const std::string& path, const std::string* source_path, Op&& operation) noexcept {
    try {
        return std::forward<Op>(operation)();
    } catch (const std::exception& e) {
        log_route_operation_error(event, fallback_event, method, path, e.what(), source_path);
        return false;
    } catch (...) {
        log_route_operation_error(event, fallback_event, method, path, "unknown", source_path);
        return false;
    }
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

    return router;
}

bool register_root_default_route(const ServerConfig& config, const std::shared_ptr<http::Router>& router) {
    if (!config.enable_root_default) {
        return true;
    }

    const std::string& root_default_path = config.root_default_path;
    if (!router->has_route_exact(http::HttpMethod::Get, root_default_path)) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("target_path", root_default_path)
            .field("error", "source_get_route_not_found")
            .field("decision", "exit_process");
        return false;
    }

    const bool root_added = router->add_route(http::HttpMethod::Get, "/", root_default_path);
    if (!root_added && !router->has_route_exact(http::HttpMethod::Get, "/")) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("target_path", root_default_path)
            .field("error", "register_route_failed")
            .field("decision", "exit_process");
        return false;
    }
    return true;
}

}  // namespace

ServerApp::ServerApp(std::span<char*> args) : startup_(args) {}

std::shared_ptr<http::Router> ServerApp::get_router() const {
    if (!startup_.ok) {
        return nullptr;
    }

    std::lock_guard lock(router_mutex_);
    if (router_ == nullptr) {
        router_ = build_default_router(startup_.config);
    }
    return router_;
}

bool ServerApp::add_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept {
    return execute_route_operation("add route failed", "add_route_failed", method, path, nullptr, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->add_route(method, path, std::move(handler));
    });
}

bool ServerApp::add_route(http::HttpMethod method, const std::string& path, const std::string& source_path) noexcept {
    return execute_route_operation("add route failed", "add_route_failed", method, path, &source_path, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->add_route(method, path, source_path);
    });
}

bool ServerApp::mod_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept {
    return execute_route_operation("mod route failed", "mod_route_failed", method, path, nullptr, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->mod_route(method, path, std::move(handler));
    });
}

bool ServerApp::mod_route(http::HttpMethod method, const std::string& path, const std::string& source_path) noexcept {
    return execute_route_operation("mod route failed", "mod_route_failed", method, path, &source_path, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->mod_route(method, path, source_path);
    });
}

bool ServerApp::del_route(http::HttpMethod method, const std::string& path) noexcept {
    return execute_route_operation("del route failed", "del_route_failed", method, path, nullptr, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->del_route(method, path);
    });
}

bool ServerApp::has_route_match(http::HttpMethod method, const std::string& path) const noexcept {
    return execute_route_operation("has route match failed", "has_route_match_failed", method, path, nullptr, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->has_route_match(method, path);
    });
}

bool ServerApp::has_route_exact(http::HttpMethod method, const std::string& path) const noexcept {
    return execute_route_operation("has route exact failed", "has_route_exact_failed", method, path, nullptr, [&]() {
        const std::shared_ptr<http::Router> router = get_router();
        if (router == nullptr) {
            return false;
        }
        return router->has_route_exact(method, path);
    });
}

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

    const std::shared_ptr<http::Router> router = get_router();
    if (router == nullptr) {
        return 1;
    }
    if (!register_root_default_route(startup_.config, router)) {
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
