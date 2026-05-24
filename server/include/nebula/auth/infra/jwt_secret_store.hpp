#ifndef NEBULA_AUTH_INFRA_JWT_SECRET_STORE_HPP
#define NEBULA_AUTH_INFRA_JWT_SECRET_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <vector>

namespace nebula::auth {

enum class JwtSecretStoreError : std::uint8_t {
    NotFound,
    OpenFailed,
    StatFailed,
    NotRegularFile,
    InsecurePermissions,
    InvalidFileSize,
    SecretTooLarge,
    ReadFailed,
    EmptyValue,
    InvalidSecretEncoding,
    WeakValue,
    CreateDirectoryFailed,
    GenerateFailed,
    WriteFailed,
};

[[nodiscard]] std::string_view to_string(JwtSecretStoreError error) noexcept;

[[nodiscard]] std::expected<std::vector<std::byte>, JwtSecretStoreError> load_or_create_jwt_secret(
    const std::filesystem::path& path);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_INFRA_JWT_SECRET_STORE_HPP
