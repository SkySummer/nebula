#include <cstddef>
#include <span>

#include "nebula/app/server_app.hpp"

int main(int argc, char* argv[]) {
    nebula::app::ServerApp app({argv, static_cast<std::size_t>(argc)});
    return app.run();
}
