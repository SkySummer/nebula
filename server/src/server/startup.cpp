#include "nebula/server/startup.hpp"

#include <format>

#include "nebula/server/main_options.hpp"

namespace nebula::server {

StartupResult::StartupResult(std::span<char*> args) {
    const MainOptionsParseResult options_result = parse_main_options(args);
    if (!options_result.ok) {
        error = std::format("parse main options failed: error={}, decision=exit_process", options_result.error);
        return;
    }

    if (!options_result.options.config_required) {
        ok = true;
        return;
    }

    config_path = options_result.options.config_path;

    const ServerConfigLoadResult config_load_result(options_result.options.config_path);
    if (!config_load_result.ok) {
        if (config_load_result.error_line == 0U) {
            error = std::format("load server config failed: path={}, error={}, decision=exit_process",
                                options_result.options.config_path.string(), config_load_result.error);
        } else {
            error = std::format("load server config failed: path={}, line={}, error={}, decision=exit_process",
                                options_result.options.config_path.string(), config_load_result.error_line,
                                config_load_result.error);
        }
        return;
    }

    config = config_load_result.config;
    config_source = config_load_result.source;
    ok = true;
}

}  // namespace nebula::server
