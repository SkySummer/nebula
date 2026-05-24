#include "nebula/server/lifecycle/lifecycle_state.hpp"

#include <utility>

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
    std::unreachable();
}

}  // namespace nebula::server
