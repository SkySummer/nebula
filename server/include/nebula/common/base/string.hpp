#ifndef NEBULA_COMMON_BASE_STRING_HPP
#define NEBULA_COMMON_BASE_STRING_HPP

#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace nebula::common {

enum class ParseNumberError : std::uint8_t {
    Empty,
    Invalid,
    OutOfRange,
};

[[nodiscard]] std::string_view to_string(ParseNumberError error) noexcept;

[[nodiscard]] std::string_view trim_ascii(std::string_view text);

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator);

template <typename NumberType>
    requires(((std::integral<NumberType> && !std::same_as<NumberType, bool>) || std::floating_point<NumberType>))
[[nodiscard]] std::expected<NumberType, ParseNumberError> parse_number(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(ParseNumberError::Empty);
    }

    NumberType value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(ParseNumberError::OutOfRange);
    }
    if (ec != std::errc() || ptr != (text.data() + text.size())) {
        return std::unexpected(ParseNumberError::Invalid);
    }
    return value;
}

}  // namespace nebula::common

#endif  // NEBULA_COMMON_BASE_STRING_HPP
