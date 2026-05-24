#ifndef NEBULA_COMMON_PLATFORM_TIME_HPP
#define NEBULA_COMMON_PLATFORM_TIME_HPP

#include <chrono>
#include <cstdint>

namespace nebula::common {

[[nodiscard]] std::chrono::seconds now_epoch_seconds();
[[nodiscard]] std::int64_t now_epoch_s();

}  // namespace nebula::common

#endif  // NEBULA_COMMON_PLATFORM_TIME_HPP
