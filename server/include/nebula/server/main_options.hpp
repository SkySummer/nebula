#ifndef NEBULA_SERVER_MAIN_OPTIONS_HPP
#define NEBULA_SERVER_MAIN_OPTIONS_HPP

#include <filesystem>
#include <span>
#include <string>

namespace nebula::server {

struct MainOptions {
    std::filesystem::path config_path;
    bool config_required = false;
};

struct MainOptionsParseResult {
    bool ok = true;
    MainOptions options;
    std::string error;
};

MainOptionsParseResult parse_main_options(std::span<char*> args);

}  // namespace nebula::server

#endif  // NEBULA_SERVER_MAIN_OPTIONS_HPP
