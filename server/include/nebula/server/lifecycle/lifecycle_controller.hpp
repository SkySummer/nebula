#ifndef NEBULA_SERVER_LIFECYCLE_LIFECYCLE_CONTROLLER_HPP
#define NEBULA_SERVER_LIFECYCLE_LIFECYCLE_CONTROLLER_HPP

#include <atomic>
#include <cstdint>
#include <string_view>

#include "nebula/server/lifecycle/lifecycle_state.hpp"

namespace nebula::server {

enum class StopRequestDecision : std::uint8_t {
    EnterStopping,
    CancelNextStart,
    Ignored,
};

[[nodiscard]] std::string_view to_string(StopRequestDecision decision) noexcept;

struct StopRequestTransition {
    StopRequestDecision decision = StopRequestDecision::Ignored;
    LifecycleState state = LifecycleState::Idle;
    LifecycleState next_state = LifecycleState::Idle;
};

class ServerLifecycleController {
public:
    [[nodiscard]] bool try_enter_starting(LifecycleState& observed_state) noexcept;
    [[nodiscard]] bool try_enter_running(LifecycleState& observed_state) noexcept;
    [[nodiscard]] bool consume_start_cancel_request() noexcept;
    [[nodiscard]] StopRequestTransition request_stop() noexcept;
    [[nodiscard]] LifecycleState state() const noexcept;
    void set_state(LifecycleState state) noexcept;

private:
    std::atomic<LifecycleState> state_ = LifecycleState::Idle;
    std::atomic_bool prestart_stop_requested_ = false;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_LIFECYCLE_LIFECYCLE_CONTROLLER_HPP
