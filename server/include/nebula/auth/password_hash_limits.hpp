#ifndef NEBULA_AUTH_PASSWORD_HASH_LIMITS_HPP
#define NEBULA_AUTH_PASSWORD_HASH_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace nebula::auth {

constexpr std::uint32_t kDefaultPasswordHashIterations = 600'000U;
constexpr std::uint32_t kMinPasswordHashIterations = 10'000U;
constexpr std::uint32_t kMaxPasswordHashIterations = 2'000'000U;

constexpr std::size_t kDefaultPasswordHashSaltBytes = 16U;

constexpr std::size_t kDefaultPasswordHashDerivedKeyBytes = 32U;
constexpr std::size_t kMaxPasswordHashDerivedKeyBytes = 128U;

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_PASSWORD_HASH_LIMITS_HPP
