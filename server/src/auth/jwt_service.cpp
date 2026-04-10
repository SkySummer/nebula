#include "nebula/auth/jwt_service.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include "nebula/common/base64.hpp"
#include "nebula/common/json.hpp"

namespace nebula::auth {

namespace {

constexpr std::string_view kJwtHeaderJson = R"({"alg":"HS256","typ":"JWT"})";

std::optional<std::string> hmac_sha256(std::string_view key, std::string_view data) {
    unsigned int digest_len = 0;
    std::vector<unsigned char> digest(static_cast<std::size_t>(::EVP_MD_size(::EVP_sha256())), 0U);
    const std::vector<unsigned char> data_bytes(data.begin(), data.end());

    unsigned char* digest_ptr = ::HMAC(::EVP_sha256(), key.data(), static_cast<int>(key.size()), data_bytes.data(),
                                       data_bytes.size(), digest.data(), &digest_len);
    if (digest_ptr == nullptr) {
        return std::nullopt;
    }

    std::string digest_text(digest_len, '\0');
    for (std::size_t idx = 0; idx < digest_text.size(); ++idx) {
        digest_text[idx] = static_cast<char>(digest[idx]);
    }
    return digest_text;
}

std::int64_t now_epoch_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::int64_t> parse_user_id(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::int64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }

        const auto digit = static_cast<std::int64_t>(ch - '0');
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    if (value <= 0) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1U;
    }
    return parts;
}

struct JwtPayload {
    std::string sub;
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
    const auto iat_it = object->find("iat");
    const auto exp_it = object->find("exp");
    if (sub_it == object->end() || iat_it == object->end() || exp_it == object->end()) {
        return std::nullopt;
    }

    const std::string* sub = sub_it->second.get_if_string();
    const std::int64_t* iat = iat_it->second.get_if_int64();
    const std::int64_t* exp = exp_it->second.get_if_int64();
    if (sub == nullptr || iat == nullptr || exp == nullptr || sub->empty()) {
        return std::nullopt;
    }

    JwtPayload payload;
    payload.sub = *sub;
    payload.iat = *iat;
    payload.exp = *exp;
    return payload;
}

}  // namespace

JwtService::JwtService(JwtConfig config) : config_(std::move(config)) {}

std::optional<std::string> JwtService::issue_access_token(std::int64_t user_id, std::int64_t now_epoch_seconds) const {
    if (config_.secret.empty() || config_.access_token_ttl_s <= 0 || user_id <= 0) {
        return std::nullopt;
    }

    const std::int64_t now_s = now_epoch_seconds >= 0 ? now_epoch_seconds : now_epoch_s();
    if (now_s > std::numeric_limits<std::int64_t>::max() - config_.access_token_ttl_s) {
        return std::nullopt;
    }
    const std::int64_t expires_s = now_s + config_.access_token_ttl_s;
    common::JsonObject payload_object;
    payload_object.emplace("sub", common::JsonValue(std::to_string(user_id)));
    payload_object.emplace("iat", common::JsonValue(now_s));
    payload_object.emplace("exp", common::JsonValue(expires_s));
    const std::string payload_json = common::dump_json(common::JsonValue(std::move(payload_object)));
    const std::string header = common::base64url_encode(kJwtHeaderJson);
    const std::string payload = common::base64url_encode(payload_json);
    const std::string signing_input = header + "." + payload;

    const std::optional<std::string> signature = hmac_sha256(config_.secret, signing_input);
    if (!signature.has_value()) {
        return std::nullopt;
    }

    return signing_input + "." + common::base64url_encode(*signature);
}

JwtVerifyResult JwtService::verify_access_token(std::string_view token, TokenClaims& claims,
                                                std::int64_t now_epoch_seconds) const {
    claims = TokenClaims{};
    if (config_.secret.empty()) {
        return JwtVerifyResult::Invalid;
    }

    const std::vector<std::string_view> parts = split(token, '.');
    if (parts.size() != 3U || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
        return JwtVerifyResult::Invalid;
    }

    const std::string expected_header = common::base64url_encode(kJwtHeaderJson);
    if (parts[0] != expected_header) {
        return JwtVerifyResult::Invalid;
    }

    const std::string signing_input = std::format("{}.{}", parts[0], parts[1]);
    const std::optional<std::string> expected_signature = hmac_sha256(config_.secret, signing_input);
    if (!expected_signature.has_value()) {
        return JwtVerifyResult::Invalid;
    }

    const std::optional<std::string> decoded_signature = common::base64url_decode_to_string(parts[2]);
    if (!decoded_signature.has_value() || decoded_signature->size() != expected_signature->size()) {
        return JwtVerifyResult::Invalid;
    }
    if (::CRYPTO_memcmp(decoded_signature->data(), expected_signature->data(), expected_signature->size()) != 0) {
        return JwtVerifyResult::Invalid;
    }

    const std::optional<std::string> payload_json = common::base64url_decode_to_string(parts[1]);
    if (!payload_json.has_value()) {
        return JwtVerifyResult::Invalid;
    }
    const std::optional<JwtPayload> payload = parse_payload_json(*payload_json);
    if (!payload.has_value()) {
        return JwtVerifyResult::Invalid;
    }

    const std::int64_t now_s = now_epoch_seconds >= 0 ? now_epoch_seconds : now_epoch_s();
    if (payload->exp <= now_s) {
        return JwtVerifyResult::Expired;
    }

    const std::optional<std::int64_t> user_id = parse_user_id(payload->sub);
    if (!user_id.has_value()) {
        return JwtVerifyResult::Invalid;
    }

    claims.user_id = *user_id;
    claims.issued_at_s = payload->iat;
    claims.expires_at_s = payload->exp;
    return JwtVerifyResult::Valid;
}

}  // namespace nebula::auth
