#ifndef NEBULA_SERVER_STARTUP_HPP
#define NEBULA_SERVER_STARTUP_HPP

#include <span>
#include <string>

#include "nebula/server/server_config.hpp"

namespace nebula::server {

struct StartupResult {
    bool ok = false;
    ServerConfig config;
    std::filesystem::path config_path;
    ServerConfigSource config_source = ServerConfigSource::Default;
    std::string error;

    StartupResult() = default;
    explicit StartupResult(std::span<char*> args);
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_STARTUP_HPP
