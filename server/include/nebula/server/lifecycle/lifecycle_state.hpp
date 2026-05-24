#ifndef NEBULA_SERVER_LIFECYCLE_LIFECYCLE_STATE_HPP
#define NEBULA_SERVER_LIFECYCLE_LIFECYCLE_STATE_HPP

#include <cstdint>
#include <string_view>

namespace nebula::server {

enum class LifecycleState : std::uint8_t {
    Idle,
    Starting,
    Running,
    Stopping,
};

[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;

}  // namespace nebula::server

#endif  // NEBULA_SERVER_LIFECYCLE_LIFECYCLE_STATE_HPP
