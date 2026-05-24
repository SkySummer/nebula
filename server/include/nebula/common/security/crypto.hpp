#ifndef NEBULA_COMMON_SECURITY_CRYPTO_HPP
#define NEBULA_COMMON_SECURITY_CRYPTO_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nebula::common {

inline constexpr std::size_t kRandomHexToken128Bytes = 16U;
inline constexpr std::size_t kRandomHexToken128Chars = kRandomHexToken128Bytes * 2U;

[[nodiscard]] std::optional<std::string> hmac_sha256(std::span<const std::byte> key, std::string_view data);
[[nodiscard]] std::optional<std::string> hmac_sha256(std::string_view key, std::string_view data);

[[nodiscard]] std::optional<std::string> generate_random_hex_token_128();

[[nodiscard]] bool is_valid_random_hex_token_128(std::string_view text);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_SECURITY_CRYPTO_HPP
