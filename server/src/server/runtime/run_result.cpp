#include "nebula/server/runtime/run_result.hpp"

#include <utility>

namespace nebula::server {

std::string_view to_string(RunResult result) noexcept {
    switch (result) {
        case RunResult::StartRejected:
            return "start_rejected";
        case RunResult::StartCanceled:
            return "start_canceled";
        case RunResult::StartFailed:
            return "start_failed";
        case RunResult::GracefulCompleted:
            return "graceful_completed";
        case RunResult::ForcedByTimeout:
            return "forced_by_timeout";
        case RunResult::FatalError:
            return "fatal_error";
        case RunResult::CleanupFailed:
            return "cleanup_failed";
    }
    std::unreachable();
}

bool is_successful_run_result(RunResult result) noexcept {
    return result == RunResult::GracefulCompleted;
}

}  // namespace nebula::server
