#ifndef NEBULA_AUTH_JWT_SECRET_STORE_HPP
#define NEBULA_AUTH_JWT_SECRET_STORE_HPP

#include <filesystem>
#include <optional>
#include <string>

namespace nebula::auth {

[[nodiscard]] std::optional<std::string> load_or_create_jwt_secret(const std::filesystem::path& path);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_JWT_SECRET_STORE_HPP
