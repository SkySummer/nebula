#include <string_view>

#include "nebula/common/logger.hpp"
#include "nebula/server/http_server.hpp"
#include "nebula/server/server_config.hpp"

int main() {
    nebula::server::ServerConfig config;
    nebula::common::Logger::instance().init(config.log_level, config.log_dir, config.log_also_stderr);
    nebula::server::HttpServer server(config);

    const nebula::server::HttpServer::RunResult run_result = server.start();
    const bool success = nebula::server::HttpServer::is_successful_run_result(run_result);
    const int exit_code = success ? 0 : 1;

    nebula::common::Logger::instance()
        .info("server process exiting")
        .field("run_result", nebula::server::HttpServer::to_string(run_result))
        .field("exit_code", exit_code)
        .field("result", success ? "success" : "failed");

    return exit_code;
}
