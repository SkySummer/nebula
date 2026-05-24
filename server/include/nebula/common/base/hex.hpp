#ifndef NEBULA_COMMON_BASE_HEX_HPP
#define NEBULA_COMMON_BASE_HEX_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace nebula::common {

[[nodiscard]] std::string bytes_to_hex(std::span<const std::uint8_t> bytes);

[[nodiscard]] bool is_valid_lower_hex_token(std::string_view text, std::size_t expected_chars);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_BASE_HEX_HPP
