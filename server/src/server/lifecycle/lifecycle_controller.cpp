#include "nebula/server/lifecycle/lifecycle_controller.hpp"

#include <utility>

namespace nebula::server {

std::string_view to_string(StopRequestDecision decision) noexcept {
    switch (decision) {
        case StopRequestDecision::EnterStopping:
            return "enter_stopping";
        case StopRequestDecision::CancelNextStart:
            return "cancel_next_start";
        case StopRequestDecision::Ignored:
            return "ignored";
    }
    std::unreachable();
}

bool ServerLifecycleController::try_enter_starting(LifecycleState& observed_state) noexcept {
    return state_.compare_exchange_strong(observed_state, LifecycleState::Starting);
}

bool ServerLifecycleController::try_enter_running(LifecycleState& observed_state) noexcept {
    return state_.compare_exchange_strong(observed_state, LifecycleState::Running);
}

bool ServerLifecycleController::consume_start_cancel_request() noexcept {
    return prestart_stop_requested_.exchange(false);
}

StopRequestTransition ServerLifecycleController::request_stop() noexcept {
    LifecycleState observed_state = state_.load();

    while (true) {
        if (observed_state == LifecycleState::Starting || observed_state == LifecycleState::Running) {
            if (state_.compare_exchange_weak(observed_state, LifecycleState::Stopping)) {
                prestart_stop_requested_.store(false);
                return {
                    .decision = StopRequestDecision::EnterStopping,
                    .state = observed_state,
                    .next_state = LifecycleState::Stopping,
                };
            }
            continue;
        }

        if (observed_state == LifecycleState::Idle) {
            prestart_stop_requested_.store(true);
            const LifecycleState reloaded_state = state_.load();
            if (reloaded_state != LifecycleState::Idle) {
                observed_state = reloaded_state;
                continue;
            }
            return {
                .decision = StopRequestDecision::CancelNextStart,
                .state = observed_state,
                .next_state = LifecycleState::Idle,
            };
        }

        return {
            .decision = StopRequestDecision::Ignored,
            .state = observed_state,
            .next_state = observed_state,
        };
    }
}

LifecycleState ServerLifecycleController::state() const noexcept {
    return state_.load();
}

void ServerLifecycleController::set_state(LifecycleState state) noexcept {
    state_.store(state);
}

}  // namespace nebula::server
