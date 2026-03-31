#ifndef NEBULA_SERVER_RUN_RESULT_HPP
#define NEBULA_SERVER_RUN_RESULT_HPP

#include <cstdint>
#include <string_view>

namespace nebula::server {

enum class RunResult : std::uint8_t {
    StartRejected,
    StartCanceled,
    StartFailed,
    GracefulCompleted,
    ForcedByTimeout,
    FatalError,
    CleanupFailed,
};

[[nodiscard]] std::string_view to_string(RunResult result) noexcept;
[[nodiscard]] bool is_successful_run_result(RunResult result) noexcept;

}  // namespace nebula::server

#endif  // NEBULA_SERVER_RUN_RESULT_HPP
