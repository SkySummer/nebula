#include <cstddef>
#include <span>

#include "nebula/server/server_app.hpp"

int main(int argc, char* argv[]) {
    nebula::server::ServerApp app({argv, static_cast<std::size_t>(argc)});
    return app.run();
}
