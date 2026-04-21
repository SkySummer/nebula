#ifndef NEBULA_APP_SERVER_APP_HPP
#define NEBULA_APP_SERVER_APP_HPP

#include <memory>
#include <span>

#include "nebula/app/startup.hpp"
#include "nebula/auth/auth_service.hpp"
#include "nebula/http/router.hpp"

namespace nebula::app {

class ServerApp {
public:
    explicit ServerApp(std::span<char*> args);

    [[nodiscard]] int run();

private:
    [[nodiscard]] std::shared_ptr<http::Router> get_router();
    [[nodiscard]] bool ensure_auth_service_initialized();
    [[nodiscard]] bool ensure_auth_routes_registered(const std::shared_ptr<http::Router>& router);
    [[nodiscard]] bool ensure_storage_routes_registered(const std::shared_ptr<http::Router>& router);

    StartupResult startup_;
    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
    bool auth_routes_registered_ = false;
    bool storage_routes_registered_ = false;
};

}  // namespace nebula::app

#endif  // NEBULA_APP_SERVER_APP_HPP
