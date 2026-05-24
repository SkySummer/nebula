#ifndef NEBULA_COMMON_CODEC_BASE64_HPP
#define NEBULA_COMMON_CODEC_BASE64_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::common {

enum class Base64DecodeError : std::uint8_t {
    InvalidLength,
    InvalidPadding,
    InvalidCharacter,
};

[[nodiscard]] std::string_view to_string(Base64DecodeError error) noexcept;

[[nodiscard]] std::string base64_encode(std::span<const std::byte> input);
[[nodiscard]] std::string base64_encode(std::string_view input);
[[nodiscard]] std::expected<std::vector<std::byte>, Base64DecodeError> base64_decode_to_bytes(std::string_view input);
[[nodiscard]] std::expected<std::string, Base64DecodeError> base64_decode_to_string(std::string_view input);

[[nodiscard]] std::string base64url_encode(std::span<const std::byte> input);
[[nodiscard]] std::string base64url_encode(std::string_view input);
[[nodiscard]] std::expected<std::vector<std::byte>, Base64DecodeError> base64url_decode_to_bytes(
    std::string_view input);
[[nodiscard]] std::expected<std::string, Base64DecodeError> base64url_decode_to_string(std::string_view input);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_CODEC_BASE64_HPP
