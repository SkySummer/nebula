#ifndef NEBULA_APP_STARTUP_HPP
#define NEBULA_APP_STARTUP_HPP

#include <filesystem>
#include <span>
#include <string>

#include "nebula/app/server_config.hpp"

namespace nebula::app {

struct StartupResult {
    bool ok = false;
    ServerConfig config;
    std::filesystem::path config_path;
    ServerConfigSource config_source = ServerConfigSource::Default;
    std::string error;

    StartupResult() = default;
    explicit StartupResult(std::span<char*> args);
};

}  // namespace nebula::app

#endif  // NEBULA_APP_STARTUP_HPP
