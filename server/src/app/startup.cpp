#include "nebula/app/startup.hpp"

#include "nebula/app/main_options.hpp"
#include "nebula/common/log/logger.hpp"

namespace nebula::app {

StartupContext startup(std::span<char*> args) {
    StartupContext result;

    const auto options_result = parse_main_options(args);
    if (!options_result.has_value()) {
        common::Logger::instance()
            .fatal("parse main options failed")
            .field("error", "main_options_invalid")
            .field("decision", "exit_process");
        return result;
    }

    if (!options_result->use_config_file) {
        result.config.normalize();
        if (!result.config.validate()) {
            common::Logger::instance()
                .fatal("load server config failed")
                .field("source", to_string(result.config_source))
                .field("error", "server_config_invalid")
                .field("decision", "exit_process");
            return result;
        }
        result.ok = true;
        return result;
    }

    result.config_path = options_result->config_path;

    const AppConfigLoadResult config_load_result = load_app_config(options_result->config_path);
    if (!config_load_result.ok) {
        common::Logger::instance()
            .fatal("load server config failed")
            .field("path", options_result->config_path)
            .field("error", "server_config_invalid")
            .field("decision", "exit_process");
        return result;
    }

    result.config = config_load_result.config;
    result.config_source = config_load_result.source;
    result.ok = true;
    return result;
}

}  // namespace nebula::app
