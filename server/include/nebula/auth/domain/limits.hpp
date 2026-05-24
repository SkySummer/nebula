#ifndef NEBULA_AUTH_DOMAIN_LIMITS_HPP
#define NEBULA_AUTH_DOMAIN_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace nebula::auth {

inline constexpr std::int64_t kDefaultAccessTokenTtlSeconds = 3'600;
inline constexpr std::int64_t kMinAccessTokenTtlSeconds = 60;
inline constexpr std::int64_t kMaxAccessTokenTtlSeconds = 2'592'000;
inline constexpr std::int64_t kAccessTokenClockSkewSeconds = 5;

inline constexpr std::uint32_t kDefaultPasswordHashIterations = 600'000U;
inline constexpr std::uint32_t kMinPasswordHashIterations = 10'000U;
inline constexpr std::uint32_t kMaxPasswordHashIterations = 2'000'000U;

inline constexpr std::size_t kDefaultPasswordHashSaltBytes = 16U;
inline constexpr std::size_t kMinPasswordHashSaltBytes = 16U;
inline constexpr std::size_t kMaxPasswordHashSaltBytes = 64U;

inline constexpr std::size_t kDefaultPasswordHashDerivedKeyBytes = 32U;
inline constexpr std::size_t kMinPasswordHashDerivedKeyBytes = 32U;
inline constexpr std::size_t kMaxPasswordHashDerivedKeyBytes = 128U;

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_DOMAIN_LIMITS_HPP
