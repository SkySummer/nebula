#ifndef NEBULA_SERVER_SERVER_APP_HPP
#define NEBULA_SERVER_SERVER_APP_HPP

#include <span>

#include "nebula/server/startup.hpp"

namespace nebula::server {

class ServerApp {
public:
    explicit ServerApp(std::span<char*> args);

    [[nodiscard]] int run() const;

private:
    StartupResult startup_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SERVER_APP_HPP
