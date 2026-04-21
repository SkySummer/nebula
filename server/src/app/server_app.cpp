#include "nebula/app/server_app.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "nebula/auth/auth_http.hpp"
#include "nebula/auth/auth_service.hpp"
#include "nebula/common/database_utils.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/postgres_connection_pool.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/server_builder.hpp"
#include "nebula/storage/storage_http.hpp"

namespace nebula::app {

namespace {

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
                .fatal(common::LogDomain::App, "register default route failed")
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
                .fatal(common::LogDomain::App, "register default route failed")
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
            .fatal(common::LogDomain::App, "register default route failed")
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
            .fatal(common::LogDomain::App, "register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("target_path", root_default_path)
            .field("error", "register_route_failed")
            .field("decision", "exit_process");
        return false;
    }
    return true;
}

bool initialize_database_pool(const ServerConfig& config) {
    const std::optional<std::string> password = common::resolve_database_password(config.database_password_env);
    if (!password.has_value()) {
        common::Logger::instance()
            .fatal(common::LogDomain::App, "database pool init failed")
            .field("error", "database_password_env_not_set")
            .field("password_env", config.database_password_env)
            .field("decision", "exit_process");
        return false;
    }

    const common::PostgresConnectionPool::InitializeStatus status =
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
    switch (status) {
        case common::PostgresConnectionPool::InitializeStatus::Initialized:
        case common::PostgresConnectionPool::InitializeStatus::AlreadyInitialized:
            return true;
        case common::PostgresConnectionPool::InitializeStatus::AlreadyInitializedWithDifferentOptions:
        case common::PostgresConnectionPool::InitializeStatus::InvalidConfig:
        case common::PostgresConnectionPool::InitializeStatus::ConnectionCreateFailed:
        case common::PostgresConnectionPool::InitializeStatus::ReplenishWorkerStartFailed:
            break;
    }
    common::Logger::instance()
        .fatal(common::LogDomain::App, "database pool init failed")
        .field("error", common::to_string(status))
        .field("decision", "exit_process");
    return false;
}

}  // namespace

ServerApp::ServerApp(std::span<char*> args) : startup_(args) {}

std::shared_ptr<http::Router> ServerApp::get_router() {
    if (!startup_.ok) {
        return nullptr;
    }

    if (router_ == nullptr) {
        router_ = build_default_router(startup_.config);
    }
    return router_;
}

bool ServerApp::ensure_auth_service_initialized() {
    if (auth_service_ != nullptr) {
        return true;
    }

    auth_service_ = auth::initialize_auth_service(startup_.config);

    if (auth_service_ == nullptr) {
        common::Logger::instance()
            .fatal(common::LogDomain::App, "register auth routes failed")
            .field("error", "auth_service_init_failed")
            .field("decision", "exit_process");
        return false;
    }
    return true;
}

bool ServerApp::ensure_auth_routes_registered(const std::shared_ptr<http::Router>& router) {
    if (router == nullptr || auth_service_ == nullptr) {
        return false;
    }

    if (auth_routes_registered_) {
        return true;
    }

    if (!auth::register_auth_routes(auth_service_, router)) {
        common::Logger::instance()
            .fatal(common::LogDomain::App, "register auth routes failed")
            .field("error", "register_auth_routes_failed")
            .field("decision", "exit_process");
        return false;
    }

    auth_routes_registered_ = true;
    return true;
}

bool ServerApp::ensure_storage_routes_registered(const std::shared_ptr<http::Router>& router) {
    if (router == nullptr || auth_service_ == nullptr) {
        return false;
    }

    if (storage_routes_registered_) {
        return true;
    }

    if (!storage::register_storage_routes(startup_.config, router)) {
        common::Logger::instance()
            .fatal(common::LogDomain::App, "register storage routes failed")
            .field("error", "register_storage_routes_failed")
            .field("decision", "exit_process");
        return false;
    }

    storage_routes_registered_ = true;
    return true;
}

int ServerApp::run() {
    if (!startup_.ok) {
        std::cerr << startup_.error << '\n';
        return 1;
    }

    common::Logger::instance().initialize(startup_.config.log_level, startup_.config.log_dir,
                                          startup_.config.log_also_stderr);

    if (startup_.config_source == ServerConfigSource::Default) {
        common::Logger::instance()
            .info(common::LogDomain::App, "server config defaults applied")
            .field("source", to_string(startup_.config_source));
    } else {
        common::Logger::instance()
            .info(common::LogDomain::App, "server config loaded")
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
    if (!ensure_auth_service_initialized()) {
        return 1;
    }
    if (!initialize_database_pool(startup_.config)) {
        return 1;
    }
    if (!ensure_auth_routes_registered(router)) {
        return 1;
    }
    if (!ensure_storage_routes_registered(router)) {
        return 1;
    }

    server::ServerRuntime runtime = server::ServerBuilder()
                                        .with_config(startup_.config)
                                        .with_router(router)
                                        .with_auth_service(auth_service_)
                                        .build();
    const server::RunResult run_result = runtime.run();
    const bool success = server::is_successful_run_result(run_result);
    const int exit_code = success ? 0 : 1;

    common::Logger::instance()
        .info(common::LogDomain::App, "server process exiting")
        .field("run_result", server::to_string(run_result))
        .field("exit_code", exit_code)
        .field("result", success ? "success" : "failed");

    return exit_code;
}

}  // namespace nebula::app
