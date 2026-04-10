#ifndef NEBULA_AUTH_JWT_SERVICE_HPP
#define NEBULA_AUTH_JWT_SERVICE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nebula::auth {

struct JwtConfig {
    std::string secret;
    std::int64_t access_token_ttl_s = 3600;
};

struct TokenClaims {
    std::int64_t user_id = 0;
    std::int64_t issued_at_s = 0;
    std::int64_t expires_at_s = 0;
};

enum class JwtVerifyResult : std::uint8_t {
    Valid,
    Invalid,
    Expired,
};

class JwtService {
public:
    explicit JwtService(JwtConfig config = {});

    [[nodiscard]] std::optional<std::string> issue_access_token(std::int64_t user_id,
                                                                std::int64_t now_epoch_seconds = -1) const;
    [[nodiscard]] JwtVerifyResult verify_access_token(std::string_view token, TokenClaims& claims,
                                                      std::int64_t now_epoch_seconds = -1) const;

private:
    JwtConfig config_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_JWT_SERVICE_HPP
