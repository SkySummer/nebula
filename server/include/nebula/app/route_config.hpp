#ifndef NEBULA_APP_ROUTE_CONFIG_HPP
#define NEBULA_APP_ROUTE_CONFIG_HPP

#include <string>

namespace nebula::app {

struct RouteConfig {
    bool enable_healthz = true;
    bool enable_echo = true;
    bool enable_root_default = true;
    std::string root_default_path = "/healthz";

    [[nodiscard]] bool validate() const;
};

}  // namespace nebula::app

#endif  // NEBULA_APP_ROUTE_CONFIG_HPP
