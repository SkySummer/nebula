#ifndef NEBULA_SERVER_SERVER_APP_HPP
#define NEBULA_SERVER_SERVER_APP_HPP

#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "nebula/http/router.hpp"
#include "nebula/server/startup.hpp"

namespace nebula::server {

class ServerApp {
public:
    explicit ServerApp(std::span<char*> args);

    bool add_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept;
    bool add_route(http::HttpMethod method, const std::string& path, const std::string& source_path) noexcept;
    bool mod_route(http::HttpMethod method, const std::string& path, http::Router::Handler handler) noexcept;
    bool mod_route(http::HttpMethod method, const std::string& path, const std::string& source_path) noexcept;
    bool del_route(http::HttpMethod method, const std::string& path) noexcept;
    [[nodiscard]] bool has_route_match(http::HttpMethod method, const std::string& path) const noexcept;
    [[nodiscard]] bool has_route_exact(http::HttpMethod method, const std::string& path) const noexcept;

    [[nodiscard]] int run() const;

private:
    [[nodiscard]] std::shared_ptr<http::Router> get_router() const;
    [[nodiscard]] bool ensure_auth_routes_registered(const std::shared_ptr<http::Router>& router) const;

    StartupResult startup_;
    mutable std::mutex router_mutex_;
    mutable std::shared_ptr<http::Router> router_;
    mutable bool auth_routes_registered_ = false;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SERVER_APP_HPP
