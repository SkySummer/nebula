#include "nebula/common/time_utils.hpp"

#include <chrono>

namespace nebula::common {

std::int64_t now_epoch_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace nebula::common
