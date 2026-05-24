#include "nebula/common/base/hex.hpp"

#include <algorithm>
#include <cctype>

namespace nebula::common {

namespace {

[[nodiscard]] bool is_lower_hex_digit(unsigned char ch) {
    return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
}

}  // namespace

std::string bytes_to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out(bytes.size() * 2U, '\0');
    for (std::size_t idx = 0; idx < bytes.size(); ++idx) {
        out[idx * 2U] = kHex[(bytes[idx] >> 4U) & 0x0FU];
        out[(idx * 2U) + 1U] = kHex[bytes[idx] & 0x0FU];
    }
    return out;
}

bool is_valid_lower_hex_token(std::string_view text, std::size_t expected_chars) {
    if (text.size() != expected_chars) {
        return false;
    }
    return std::ranges::all_of(text, is_lower_hex_digit);
}

}  // namespace nebula::common
