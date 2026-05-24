#include "nebula/auth/infra/jwt_service.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/common/base/arithmetic.hpp"
#include "nebula/common/base/string.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula/common/codec/json.hpp"
#include "nebula/common/platform/time.hpp"
#include "nebula/common/security/crypto.hpp"

namespace nebula::auth {

namespace {

constexpr std::string_view kJwtHeaderJson = R"({"alg":"HS256","typ":"JWT"})";

std::optional<std::int64_t> parse_user_id(std::string_view text) {
    const auto value = common::parse_number<std::int64_t>(text);
    if (!value.has_value() || *value <= 0) {
        return std::nullopt;
    }
    return *value;
}

struct JwtPayload {
    std::string sub;
    std::int64_t ver = 0;
    std::int64_t iat = 0;
    std::int64_t exp = 0;
};

std::optional<JwtPayload> parse_payload_json(std::string_view payload_json) {
    const common::JsonParseResult parsed = common::parse_json(payload_json);
    if (!parsed.ok) {
        return std::nullopt;
    }

    const common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }

    const auto sub_it = object->find("sub");
    const auto ver_it = object->find("ver");
    const auto iat_it = object->find("iat");
    const auto exp_it = object->find("exp");
    if (sub_it == object->end() || ver_it == object->end() || iat_it == object->end() || exp_it == object->end()) {
        return std::nullopt;
    }

    const std::string* sub = sub_it->second.get_if_string();
    const std::int64_t* ver = ver_it->second.get_if_int64();
    const std::int64_t* iat = iat_it->second.get_if_int64();
    const std::int64_t* exp = exp_it->second.get_if_int64();
    if (sub == nullptr || ver == nullptr || iat == nullptr || exp == nullptr || sub->empty() || *ver <= 0) {
        return std::nullopt;
    }

    JwtPayload payload;
    payload.sub = *sub;
    payload.ver = *ver;
    payload.iat = *iat;
    payload.exp = *exp;
    return payload;
}

std::optional<JwtVerifyError> validate_access_token_times(const JwtPayload& payload, std::int64_t now_s,
                                                          std::int64_t access_token_ttl_s) {
    if (payload.iat <= 0) {
        return JwtVerifyError::Invalid;
    }

    const std::int64_t max_allowed_iat = common::saturating_add(now_s, kAccessTokenClockSkewSeconds);
    if (payload.iat > max_allowed_iat) {
        return JwtVerifyError::Invalid;
    }

    if (payload.exp <= payload.iat) {
        return JwtVerifyError::Invalid;
    }

    const std::int64_t token_ttl_s = common::saturating_sub(payload.exp, payload.iat);
    if (token_ttl_s > access_token_ttl_s) {
        return JwtVerifyError::Invalid;
    }

    if (payload.exp <= now_s) {
        return JwtVerifyError::Expired;
    }

    return std::nullopt;
}

}  // namespace

JwtService::JwtService(JwtConfig config) : config_(std::move(config)) {
    if (config_.secret.empty()) {
        throw std::invalid_argument("jwt_secret_missing");
    }
    if (config_.access_token_ttl_s < kMinAccessTokenTtlSeconds ||
        config_.access_token_ttl_s > kMaxAccessTokenTtlSeconds) {
        throw std::invalid_argument("jwt_access_token_ttl_invalid");
    }
}

std::optional<std::string> JwtService::issue_access_token(std::int64_t user_id, std::int64_t token_version) const {
    if (user_id <= 0 || token_version <= 0) {
        return std::nullopt;
    }

    const std::int64_t now_s = common::now_epoch_s();
    if (now_s > std::numeric_limits<std::int64_t>::max() - config_.access_token_ttl_s) {
        return std::nullopt;
    }
    const std::int64_t expires_s = now_s + config_.access_token_ttl_s;

    common::JsonObject payload_object;
    payload_object.emplace("sub", std::to_string(user_id));
    payload_object.emplace("ver", token_version);
    payload_object.emplace("iat", now_s);
    payload_object.emplace("exp", expires_s);
    const std::string payload_json = common::dump_json(common::JsonValue(std::move(payload_object)));

    const std::string header = common::base64url_encode(kJwtHeaderJson);
    const std::string payload = common::base64url_encode(payload_json);
    const std::string signing_input = std::format("{}.{}", header, payload);

    const std::optional<std::string> signature = common::hmac_sha256(config_.secret, signing_input);
    if (!signature.has_value()) {
        return std::nullopt;
    }

    return std::format("{}.{}", signing_input, common::base64url_encode(*signature));
}

std::expected<TokenClaims, JwtVerifyError> JwtService::verify_access_token(std::string_view token) const {
    const std::vector<std::string_view> parts = common::split(token, '.');
    if (parts.size() != 3U || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    const std::string expected_header = common::base64url_encode(kJwtHeaderJson);
    if (parts[0] != expected_header) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    const std::string signing_input = std::format("{}.{}", parts[0], parts[1]);
    const std::optional<std::string> expected_signature = common::hmac_sha256(config_.secret, signing_input);
    if (!expected_signature.has_value()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    const auto decoded_signature = common::base64url_decode_to_string(parts[2]);
    if (!decoded_signature.has_value() || decoded_signature->size() != expected_signature->size()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }
    if (::CRYPTO_memcmp(decoded_signature->data(), expected_signature->data(), expected_signature->size()) != 0) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    const auto payload_json = common::base64url_decode_to_string(parts[1]);
    if (!payload_json.has_value()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }
    const std::optional<JwtPayload> payload = parse_payload_json(*payload_json);
    if (!payload.has_value()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    const std::int64_t now_s = common::now_epoch_s();
    const std::optional<JwtVerifyError> time_validation_result =
        validate_access_token_times(*payload, now_s, config_.access_token_ttl_s);
    if (time_validation_result.has_value()) {
        return std::unexpected(*time_validation_result);
    }

    const std::optional<std::int64_t> user_id = parse_user_id(payload->sub);
    if (!user_id.has_value()) {
        return std::unexpected(JwtVerifyError::Invalid);
    }

    return TokenClaims{
        .user_id = *user_id,
        .token_version = payload->ver,
        .issued_at_s = payload->iat,
        .expires_at_s = payload->exp,
    };
}

}  // namespace nebula::auth
