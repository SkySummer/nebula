#ifndef NEBULA_APP_APP_CONFIG_HPP
#define NEBULA_APP_APP_CONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "nebula/app/route_config.hpp"
#include "nebula/auth/bootstrap/config.hpp"
#include "nebula/common/log/types.hpp"
#include "nebula/database/config.hpp"
#include "nebula/http/config/http_limits_config.hpp"
#include "nebula/server/config/config.hpp"
#include "nebula/server/config/timeouts_config.hpp"
#include "nebula/storage/bootstrap/config.hpp"

namespace nebula::app {

struct AppConfig {
    server::ServerConfig server;
    common::LoggerConfig logger;
    server::ServerTimeoutConfig timeouts;
    http::HttpLimitsConfig limits;
    RouteConfig routes;
    database::DatabaseConfig database;
    auth::AuthConfig auth;
    storage::StorageConfig storage;

    AppConfig& normalize() &;
    AppConfig&& normalize() &&;

    [[nodiscard]] bool validate() const;
};

enum class AppConfigSource : std::uint8_t {
    Default,
    File,
};

[[nodiscard]] std::string_view to_string(AppConfigSource source) noexcept;

struct AppConfigLoadResult {
    bool ok = true;
    AppConfig config;
    AppConfigSource source = AppConfigSource::Default;
};

[[nodiscard]] AppConfigLoadResult load_app_config(const std::filesystem::path& path);

}  // namespace nebula::app

#endif  // NEBULA_APP_APP_CONFIG_HPP
