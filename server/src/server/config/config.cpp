#include "nebula/server/config/config.hpp"

#include <limits>

#include "nebula/common/log/logger.hpp"

namespace nebula::server {

ServerConfig& ServerConfig::normalize() & {
    if (sub_reactor_count == 0U) {
        sub_reactor_count = default_sub_reactor_count();
    }
    if (worker_thread_count == 0U) {
        worker_thread_count = default_worker_thread_count();
    }
    return *this;
}

ServerConfig&& ServerConfig::normalize() && {
    normalize();
    return std::move(*this);
}

bool ServerConfig::validate() const {
    bool ok = true;
    if (port == 0U) {
        common::Logger::instance()
            .error("server config value out of range")
            .field("key", "port")
            .field("value", 0)
            .field("min_value", 1)
            .field("max_value", std::numeric_limits<std::uint16_t>::max());
        ok = false;
    }
    if (backlog < 0) {
        common::Logger::instance()
            .error("server config value out of range")
            .field("key", "backlog")
            .field("value", backlog)
            .field("min_value", 0)
            .field("max_value", std::numeric_limits<int>::max());
        ok = false;
    }
    if (max_connections == 0U || max_connections > kMaxServerMaxConnections) {
        common::Logger::instance()
            .error("server config value out of range")
            .field("key", "max_connections")
            .field("value", max_connections)
            .field("min_value", 1)
            .field("max_value", kMaxServerMaxConnections);
        ok = false;
    }

    const std::size_t effective_sub_reactor_count =
        sub_reactor_count == 0U ? default_sub_reactor_count() : sub_reactor_count;
    if (effective_sub_reactor_count > kMaxServerSubReactorCount) {
        common::Logger::instance()
            .error("server config value out of range")
            .field("key", "sub_reactor_count")
            .field("value", sub_reactor_count)
            .field("min_value", 0)
            .field("max_value", kMaxServerSubReactorCount);
        ok = false;
    }

    const std::size_t effective_worker_thread_count =
        worker_thread_count == 0U ? default_worker_thread_count() : worker_thread_count;
    if (effective_worker_thread_count > kMaxServerWorkerThreadCount) {
        common::Logger::instance()
            .error("server config value out of range")
            .field("key", "worker_thread_count")
            .field("value", worker_thread_count)
            .field("min_value", 0)
            .field("max_value", kMaxServerWorkerThreadCount);
        ok = false;
    }
    return ok;
}

}  // namespace nebula::server
