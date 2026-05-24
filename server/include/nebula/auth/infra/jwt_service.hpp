#ifndef NEBULA_AUTH_INFRA_JWT_SERVICE_HPP
#define NEBULA_AUTH_INFRA_JWT_SERVICE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::auth {

struct JwtConfig {
    std::vector<std::byte> secret;
    std::int64_t access_token_ttl_s = 3600;
};

struct TokenClaims {
    std::int64_t user_id = 0;
    std::int64_t token_version = 0;
    std::int64_t issued_at_s = 0;
    std::int64_t expires_at_s = 0;
};

enum class JwtVerifyError : std::uint8_t {
    Invalid,
    Expired,
};

class JwtService {
public:
    explicit JwtService(JwtConfig config);

    [[nodiscard]] std::optional<std::string> issue_access_token(std::int64_t user_id, std::int64_t token_version) const;
    [[nodiscard]] std::expected<TokenClaims, JwtVerifyError> verify_access_token(std::string_view token) const;

private:
    JwtConfig config_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_INFRA_JWT_SERVICE_HPP
