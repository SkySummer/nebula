#include "nebula/common/security/crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "nebula/common/base/hex.hpp"

namespace nebula::common {

namespace {

std::optional<std::string> hmac_sha256_impl(const void* key_data, int key_size, std::string_view data) {
    unsigned int digest_len = 0;
    std::vector<std::uint8_t> digest(static_cast<std::size_t>(::EVP_MD_size(::EVP_sha256())), 0U);
    const std::vector<std::uint8_t> data_bytes(data.begin(), data.end());
    std::uint8_t* digest_ptr =
        ::HMAC(::EVP_sha256(), key_data, key_size, data_bytes.data(), data_bytes.size(), digest.data(), &digest_len);
    if (digest_ptr == nullptr || digest_len == 0U) {
        return std::nullopt;
    }

    return std::string(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(digest_len));
}

}  // namespace

std::optional<std::string> hmac_sha256(std::span<const std::byte> key, std::string_view data) {
    return hmac_sha256_impl(key.data(), static_cast<int>(key.size()), data);
}

std::optional<std::string> hmac_sha256(std::string_view key, std::string_view data) {
    return hmac_sha256_impl(key.data(), static_cast<int>(key.size()), data);
}

std::optional<std::string> generate_random_hex_token_128() {
    std::array<std::uint8_t, kRandomHexToken128Bytes> random{};
    if (::RAND_priv_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return std::nullopt;
    }
    return bytes_to_hex(random);
}

bool is_valid_random_hex_token_128(std::string_view text) {
    return is_valid_lower_hex_token(text, kRandomHexToken128Chars);
}

}  // namespace nebula::common
