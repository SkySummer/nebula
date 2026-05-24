#include "nebula/app/server_app.hpp"

#include <memory>
#include <string>
#include <utility>

#include "nebula/auth/bootstrap/module.hpp"
#include "nebula/common/codec/json.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula/http/codec/response_writer.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/server/runtime/builder.hpp"
#include "nebula/storage/bootstrap/module.hpp"

namespace nebula::app {

namespace {

std::shared_ptr<http::Router> build_default_router(const AppConfig& config) {
    auto router = std::make_shared<http::Router>();

    if (config.routes.enable_healthz) {
        const bool added = router->add_route(http::HttpMethod::Get, "/healthz", [](const http::RouteContext&) {
            common::JsonObject data;
            data.emplace("status", "ok");
            return http::make_api_success_response(std::move(data));
        });
        if (!added) {
            common::Logger::instance()
                .fatal("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Get))
                .field("path", "/healthz")
                .field("decision", "exit_process");
            return nullptr;
        }
    }

    if (config.routes.enable_echo) {
        const bool added = router->add_route(http::HttpMethod::Post, "/echo", [](const http::RouteContext& context) {
            common::JsonObject data;
            data.emplace("request_body", context.request.body);
            return http::make_api_success_response(std::move(data));
        });
        if (!added) {
            common::Logger::instance()
                .fatal("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Post))
                .field("path", "/echo")
                .field("decision", "exit_process");
            return nullptr;
        }
    }

    if (config.routes.enable_root_default) {
        const std::string& root_default_path = config.routes.root_default_path;
        if (!router->has_route_exact(http::HttpMethod::Get, root_default_path)) {
            common::Logger::instance()
                .fatal("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Get))
                .field("path", "/")
                .field("target_path", root_default_path)
                .field("error", "source_get_route_not_found")
                .field("decision", "exit_process");
            return nullptr;
        }

        const bool added = router->add_route(http::HttpMethod::Get, "/", root_default_path);
        if (!added) {
            common::Logger::instance()
                .fatal("register default route failed")
                .field("method", http::to_string(http::HttpMethod::Get))
                .field("path", "/")
                .field("target_path", root_default_path)
                .field("error", "register_route_failed")
                .field("decision", "exit_process");
            return nullptr;
        }
    }

    return router;
}

}  // namespace

ServerApp::ServerApp(std::span<char*> args) : startup_(startup(args)) {}

int ServerApp::run() {
    if (run_started_.exchange(true)) {
        common::Logger::instance()
            .warn("server app run rejected")
            .field("error", "already_started")
            .field("decision", "return_failed");
        return 1;
    }

    if (!startup_.ok) {
        return 1;
    }

    common::Logger::instance().initialize(startup_.config.logger);

    if (startup_.config_source == AppConfigSource::Default) {
        common::Logger::instance()
            .info("server config defaults applied")
            .field("source", to_string(startup_.config_source));
    } else {
        common::Logger::instance()
            .info("server config loaded")
            .field("source", to_string(startup_.config_source))
            .field("path", startup_.config_path);
    }

    router_ = build_default_router(startup_.config);
    if (router_ == nullptr) {
        return 1;
    }

    database_pool_ = database::ConnectionPool::create(startup_.config.database);
    if (database_pool_ == nullptr) {
        common::Logger::instance()
            .fatal("database pool init failed")
            .field("error", "pool_not_initialized")
            .field("decision", "exit_process");
        return 1;
    }

    auth_module_ = auth::AuthModule::create(auth::AuthModule::Params{
        .config = &startup_.config.auth,
        .database_pool = database_pool_,
        .router = router_,
    });
    if (auth_module_ == nullptr) {
        return 1;
    }

    storage_module_ = storage::StorageModule::create(storage::StorageModule::Params{
        .config = &startup_.config.storage,
        .limits = &startup_.config.limits,
        .database_pool = database_pool_,
        .router = router_,
    });
    if (storage_module_ == nullptr) {
        return 1;
    }

    server::ServerRuntime runtime = server::ServerBuilder()
                                        .with_config(startup_.config)
                                        .with_router(router_)
                                        .with_auth_service(auth_module_->service())
                                        .build();
    const server::RunResult run_result = runtime.run();
    const bool success = server::is_successful_run_result(run_result);
    const int exit_code = success ? 0 : 1;

    common::Logger::instance()
        .info("server process exiting")
        .field("run_result", server::to_string(run_result))
        .field("exit_code", exit_code)
        .field("result", success ? "success" : "failed");

    return exit_code;
}

}  // namespace nebula::app
