#include "nebula/server/main_options.hpp"

#include <cstddef>
#include <format>
#include <string_view>

namespace nebula::server {

MainOptionsParseResult parse_main_options(std::span<char*> args) {
    MainOptionsParseResult result;

    if (args.empty()) {
        result.ok = false;
        result.error = "invalid_argv";
        return result;
    }

    for (std::size_t idx = 1; idx < args.size(); ++idx) {
        const std::string_view arg = args[idx] == nullptr ? std::string_view{} : std::string_view(args[idx]);

        if (arg == "--config") {
            if (result.options.config_required) {
                result.ok = false;
                result.error = "duplicate_config_argument";
                return result;
            }
            if ((idx + 1U) >= args.size() || args[idx + 1U] == nullptr) {
                result.ok = false;
                result.error = "missing_config_path";
                return result;
            }
            if (std::string_view(args[idx + 1U]).empty()) {
                result.ok = false;
                result.error = "empty_config_path";
                return result;
            }

            result.options.config_path = args[idx + 1U];
            result.options.config_required = true;
            ++idx;
            continue;
        }

        result.ok = false;
        result.error = std::format("unknown_argument:{}", arg);
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace nebula::server
