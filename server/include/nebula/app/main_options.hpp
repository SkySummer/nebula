#ifndef NEBULA_APP_MAIN_OPTIONS_HPP
#define NEBULA_APP_MAIN_OPTIONS_HPP

#include <filesystem>
#include <optional>
#include <span>

namespace nebula::app {

struct MainOptions {
    bool use_config_file = false;
    std::filesystem::path config_path;
};

std::optional<MainOptions> parse_main_options(std::span<char*> args);

}  // namespace nebula::app

#endif  // NEBULA_APP_MAIN_OPTIONS_HPP
