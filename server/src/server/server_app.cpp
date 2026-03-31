#include "nebula/server/server_app.hpp"

#include <iostream>
#include <memory>

#include "nebula/common/logger.hpp"
#include "nebula/http/router.hpp"
#include "nebula/server/http_server.hpp"

namespace nebula::server {

namespace {

std::shared_ptr<http::Router> build_default_router() {
    auto router = std::make_shared<http::Router>();

    const bool root_added = router->add_route(http::HttpMethod::Get, "/", [](const http::HttpRequest&) {
        http::HttpResponse response;
        response.status = http::HttpStatus::TemporaryRedirect;
        response.headers.emplace("Location", "/healthz");
        return response;
    });
    if (!root_added) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/")
            .field("decision", "exit_process");
        return nullptr;
    }

    const bool healthz_added = router->add_route(http::HttpMethod::Get, "/healthz", [](const http::HttpRequest&) {
        http::HttpResponse response;
        response.status = http::HttpStatus::OK;
        response.headers.emplace("Content-Type", "application/json");
        response.body = R"({"status":"ok"})";
        return response;
    });
    if (!healthz_added) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Get))
            .field("path", "/healthz")
            .field("decision", "exit_process");
        return nullptr;
    }

    const bool echo_added = router->add_route(http::HttpMethod::Post, "/echo", [](const http::HttpRequest& request) {
        http::HttpResponse response;
        response.status = http::HttpStatus::OK;
        response.headers.emplace("Content-Type", "application/json");
        response.body = request.body;
        return response;
    });
    if (!echo_added) {
        common::Logger::instance()
            .error("register default route failed")
            .field("method", http::to_string(http::HttpMethod::Post))
            .field("path", "/echo")
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

    const std::shared_ptr<http::Router> router = build_default_router();
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
