#include "nebula/app/main_options.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

#include "nebula/common/log/logger.hpp"

namespace nebula::app {

std::optional<MainOptions> parse_main_options(std::span<char*> args) {
    if (args.empty()) {
        common::Logger::instance().error("parse main options failed").field("error", "invalid_argv");
        return std::nullopt;
    }

    MainOptions result;
    for (std::size_t idx = 1; idx < args.size(); ++idx) {
        const std::string_view arg = args[idx] == nullptr ? std::string_view{} : std::string_view(args[idx]);

        if (arg == "--config") {
            if (result.use_config_file) {
                common::Logger::instance()
                    .error("main option invalid")
                    .field("arg", "--config")
                    .field("error", "duplicate_argument");
                return std::nullopt;
            }
            if ((idx + 1U) >= args.size() || args[idx + 1U] == nullptr) {
                common::Logger::instance()
                    .error("main option invalid")
                    .field("arg", "--config")
                    .field("error", "missing_value");
                return std::nullopt;
            }
            if (std::string_view(args[idx + 1U]).empty()) {
                common::Logger::instance()
                    .error("main option invalid")
                    .field("arg", "--config")
                    .field("error", "empty_value");
                return std::nullopt;
            }

            result.use_config_file = true;
            result.config_path = args[idx + 1U];
            ++idx;
            continue;
        }

        common::Logger::instance().error("main option invalid").field("arg", arg).field("error", "unknown_argument");
        return std::nullopt;
    }

    return result;
}

}  // namespace nebula::app
