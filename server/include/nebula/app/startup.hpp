#ifndef NEBULA_APP_STARTUP_HPP
#define NEBULA_APP_STARTUP_HPP

#include <filesystem>
#include <span>

#include "nebula/app/app_config.hpp"

namespace nebula::app {

struct StartupContext {
    bool ok = false;
    AppConfig config;
    AppConfigSource config_source = AppConfigSource::Default;
    std::filesystem::path config_path;
};

[[nodiscard]] StartupContext startup(std::span<char*> args);

}  // namespace nebula::app

#endif  // NEBULA_APP_STARTUP_HPP
