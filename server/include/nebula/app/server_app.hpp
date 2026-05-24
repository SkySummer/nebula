#ifndef NEBULA_APP_SERVER_APP_HPP
#define NEBULA_APP_SERVER_APP_HPP

#include <atomic>
#include <memory>
#include <span>

#include "nebula/app/startup.hpp"
#include "nebula/auth/bootstrap/module.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/storage/bootstrap/module.hpp"

namespace nebula::app {

class ServerApp {
public:
    explicit ServerApp(std::span<char*> args);

    [[nodiscard]] int run();

private:
    StartupContext startup_;
    std::atomic_bool run_started_ = false;
    std::shared_ptr<http::Router> router_;
    std::shared_ptr<database::ConnectionPool> database_pool_;
    std::unique_ptr<auth::AuthModule> auth_module_;
    std::unique_ptr<storage::StorageModule> storage_module_;
};

}  // namespace nebula::app

#endif  // NEBULA_APP_SERVER_APP_HPP
