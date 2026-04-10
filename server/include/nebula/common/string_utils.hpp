#ifndef NEBULA_COMMON_STRING_UTILS_HPP
#define NEBULA_COMMON_STRING_UTILS_HPP

#include <string_view>

namespace nebula::common {

[[nodiscard]] std::string_view trim_ascii(std::string_view text);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_STRING_UTILS_HPP
