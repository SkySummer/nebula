#include "nebula/server/server_app.hpp"

#include <iostream>

#include "nebula/common/logger.hpp"
#include "nebula/server/http_server.hpp"

namespace nebula::server {

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

    HttpServer server(startup_.config);
    const HttpServer::RunResult run_result = server.start();
    const bool success = HttpServer::is_successful_run_result(run_result);
    const int exit_code = success ? 0 : 1;

    common::Logger::instance()
        .info("server process exiting")
        .field("run_result", HttpServer::to_string(run_result))
        .field("exit_code", exit_code)
        .field("result", success ? "success" : "failed");

    return exit_code;
}

}  // namespace nebula::server
