#include "nebula/auth/http/authentication.hpp"

#include <cctype>
#include <string>
#include <string_view>

#include "nebula/common/base/string.hpp"

namespace nebula::auth {

namespace {

bool starts_with_ignore_case_ascii(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < prefix.size(); ++idx) {
        const auto left = static_cast<unsigned char>(text[idx]);
        const auto right = static_cast<unsigned char>(prefix[idx]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

std::string extract_bearer_token(const http::HeaderMap& headers) {
    const auto auth_it = headers.find("authorization");
    if (auth_it == headers.end()) {
        return {};
    }

    const std::string_view value = common::trim_ascii(auth_it->second);
    constexpr std::string_view k_prefix = "Bearer ";
    if (!starts_with_ignore_case_ascii(value, k_prefix)) {
        return {};
    }
    return std::string(common::trim_ascii(value.substr(k_prefix.size())));
}

}  // namespace

std::expected<AuthenticateResult, AuthError> authenticate_http_request(const std::shared_ptr<AuthService>& auth_service,
                                                                       const http::HeaderMap& headers) {
    if (auth_service == nullptr) {
        return std::unexpected(AuthError::InternalError);
    }
    const std::string token = extract_bearer_token(headers);
    return auth_service->authenticate_access_token(token);
}

}  // namespace nebula::auth
