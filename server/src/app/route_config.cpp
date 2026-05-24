#include "nebula/app/route_config.hpp"

#include "nebula/common/log/logger.hpp"

namespace nebula::app {

bool RouteConfig::validate() const {
    if (!enable_root_default) {
        return true;
    }
    if (root_default_path.empty()) {
        common::Logger::instance()
            .error("route config value invalid")
            .field("key", "root_default_path")
            .field("value", root_default_path)
            .field("error", "empty_path");
        return false;
    }
    bool ok = true;
    if (root_default_path.front() != '/') {
        common::Logger::instance()
            .error("route config value invalid")
            .field("key", "root_default_path")
            .field("value", root_default_path)
            .field("error", "must_start_with_slash");
        ok = false;
    }
    if (root_default_path == "/") {
        common::Logger::instance()
            .error("route config value invalid")
            .field("key", "root_default_path")
            .field("value", root_default_path)
            .field("error", "self_mapping_not_allowed");
        ok = false;
    }
    if (root_default_path.find('{') != std::string::npos || root_default_path.find('}') != std::string::npos) {
        common::Logger::instance()
            .error("route config value invalid")
            .field("key", "root_default_path")
            .field("value", root_default_path)
            .field("error", "path_template_not_allowed");
        ok = false;
    }
    return ok;
}

}  // namespace nebula::app
