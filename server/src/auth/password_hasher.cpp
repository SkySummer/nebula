#include "nebula/auth/password_hasher.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "nebula/common/base64.hpp"

namespace nebula::auth {

namespace {

constexpr std::string_view kPasswordHashAlgorithm = "pbkdf2_sha256";

bool is_iterations_in_allowed_range(std::uint32_t iterations) {
    return iterations >= kMinPasswordHashIterations && iterations <= kMaxPasswordHashIterations;
}

bool derive_pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt, std::uint32_t iterations,
                          std::size_t derived_key_bytes, std::vector<std::uint8_t>& out) {
    if (salt.empty() || derived_key_bytes == 0U || iterations == 0U) {
        return false;
    }
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        salt.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        derived_key_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    out.assign(derived_key_bytes, 0U);
    const int ok = ::PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                                       static_cast<int>(salt.size()), static_cast<int>(iterations), ::EVP_sha256(),
                                       static_cast<int>(derived_key_bytes), out.data());
    return ok == 1;
}

}  // namespace

PasswordHasher::PasswordHasher(PasswordHashConfig config) : config_(config) {}

std::optional<PasswordHashValue> PasswordHasher::hash_password(std::string_view password) const {
    if (config_.salt_bytes == 0U || config_.derived_key_bytes == 0U) {
        return std::nullopt;
    }
    if (!is_iterations_in_allowed_range(config_.iterations) ||
        config_.derived_key_bytes > kMaxPasswordHashDerivedKeyBytes) {
        return std::nullopt;
    }

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
        .salt = common::base64url_encode(salt),
        .derived_key = common::base64url_encode(derived),
    };
}

bool PasswordHasher::verify_password(std::string_view password, const PasswordHashValue& value) {
    if (value.algorithm != kPasswordHashAlgorithm) {
        return false;
    }
    if (!is_iterations_in_allowed_range(value.iterations)) {
        return false;
    }

    const std::optional<std::vector<std::uint8_t>> salt = common::base64url_decode_to_bytes(value.salt);
    const std::optional<std::vector<std::uint8_t>> expected = common::base64url_decode_to_bytes(value.derived_key);
    if (!salt.has_value() || !expected.has_value() || salt->empty() || expected->empty()) {
        return false;
    }
    if (expected->size() > kMaxPasswordHashDerivedKeyBytes) {
        return false;
    }

    std::vector<std::uint8_t> actual;
    if (!derive_pbkdf2_sha256(password, *salt, value.iterations, expected->size(), actual)) {
        return false;
    }

    return ::CRYPTO_memcmp(actual.data(), expected->data(), expected->size()) == 0;
}

}  // namespace nebula::auth
