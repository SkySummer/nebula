#include "nebula/common/platform/time.hpp"

#include <chrono>

namespace nebula::common {

std::chrono::seconds now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
}

std::int64_t now_epoch_s() {
    return now_epoch_seconds().count();
}

}  // namespace nebula::common
