#include "nebula/auth/infra/password_hasher.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/common/codec/base64.hpp"

namespace nebula::auth {

namespace {

constexpr std::string_view kPasswordHashAlgorithm = "pbkdf2_sha256";

std::vector<std::uint8_t> to_unsigned_char_buffer(std::span<const std::byte> input) {
    std::vector<std::uint8_t> out;
    out.reserve(input.size());
    for (const std::byte byte : input) {
        out.push_back(std::to_integer<std::uint8_t>(byte));
    }
    return out;
}

bool derive_pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt, std::uint32_t iterations,
                          std::size_t derived_key_bytes, std::vector<std::uint8_t>& out) {
    if (salt.empty() || derived_key_bytes == 0U || iterations == 0U) {
        return false;
    }
    if (std::cmp_greater(password.size(), std::numeric_limits<int>::max()) ||
        std::cmp_greater(salt.size(), std::numeric_limits<int>::max()) ||
        std::cmp_greater(iterations, std::numeric_limits<int>::max()) ||
        std::cmp_greater(derived_key_bytes, std::numeric_limits<int>::max())) {
        return false;
    }

    out.assign(derived_key_bytes, 0U);
    const int ok = ::PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                                       static_cast<int>(salt.size()), static_cast<int>(iterations), ::EVP_sha256(),
                                       static_cast<int>(derived_key_bytes), out.data());
    return ok == 1;
}

}  // namespace

PasswordHasher::PasswordHasher(PasswordHashConfig config) : config_(config) {
    if (config_.iterations < kMinPasswordHashIterations || config_.iterations > kMaxPasswordHashIterations) {
        throw std::invalid_argument("password_hash_iterations_invalid");
    }
    if (config_.salt_bytes < kMinPasswordHashSaltBytes || config_.salt_bytes > kMaxPasswordHashSaltBytes) {
        throw std::invalid_argument("password_hash_salt_bytes_invalid");
    }
    if (config_.derived_key_bytes < kMinPasswordHashDerivedKeyBytes ||
        config_.derived_key_bytes > kMaxPasswordHashDerivedKeyBytes) {
        throw std::invalid_argument("password_hash_derived_key_bytes_invalid");
    }
}

std::optional<PasswordHashValue> PasswordHasher::hash_password(std::string_view password) const {
    std::vector<std::uint8_t> salt(config_.salt_bytes, 0U);
    if (::RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> derived;
    if (!derive_pbkdf2_sha256(password, salt, config_.iterations, config_.derived_key_bytes, derived)) {
        return std::nullopt;
    }

    return PasswordHashValue{
        .algorithm = std::string(kPasswordHashAlgorithm),
        .iterations = config_.iterations,
        .salt = common::base64url_encode(std::as_bytes(std::span{salt})),
        .derived_key = common::base64url_encode(std::as_bytes(std::span{derived})),
    };
}

bool PasswordHasher::verify_password(std::string_view password, const PasswordHashValue& value) {
    if (value.algorithm != kPasswordHashAlgorithm) {
        return false;
    }
    if (value.iterations < kMinPasswordHashIterations || value.iterations > kMaxPasswordHashIterations) {
        return false;
    }

    const auto salt_bytes = common::base64url_decode_to_bytes(value.salt);
    if (!salt_bytes.has_value() || salt_bytes->size() < kMinPasswordHashSaltBytes ||
        salt_bytes->size() > kMaxPasswordHashSaltBytes) {
        return false;
    }

    const auto expected = common::base64url_decode_to_bytes(value.derived_key);
    if (!expected.has_value() || expected->size() < kMinPasswordHashDerivedKeyBytes ||
        expected->size() > kMaxPasswordHashDerivedKeyBytes) {
        return false;
    }

    const std::vector<std::uint8_t> salt = to_unsigned_char_buffer(*salt_bytes);
    std::vector<std::uint8_t> actual;
    if (!derive_pbkdf2_sha256(password, salt, value.iterations, expected->size(), actual)) {
        return false;
    }

    return ::CRYPTO_memcmp(actual.data(), expected->data(), expected->size()) == 0;
}

}  // namespace nebula::auth
