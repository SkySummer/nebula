#ifndef NEBULA_COMMON_BASE64_HPP
#define NEBULA_COMMON_BASE64_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::common {

[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> input);
[[nodiscard]] std::string base64_encode(std::string_view input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode_to_bytes(std::string_view input);
[[nodiscard]] std::optional<std::string> base64_decode_to_string(std::string_view input);

[[nodiscard]] std::string base64url_encode(std::span<const std::uint8_t> input);
[[nodiscard]] std::string base64url_encode(std::string_view input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64url_decode_to_bytes(std::string_view input);
[[nodiscard]] std::optional<std::string> base64url_decode_to_string(std::string_view input);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_BASE64_HPP
