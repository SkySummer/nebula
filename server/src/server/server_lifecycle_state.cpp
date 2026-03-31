#include "nebula/server/server_lifecycle_state.hpp"

namespace nebula::server {

std::string_view to_string(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Idle:
            return "idle";
        case LifecycleState::Starting:
            return "starting";
        case LifecycleState::Running:
            return "running";
        case LifecycleState::Stopping:
            return "stopping";
    }
    return "unknown";
}

}  // namespace nebula::server
