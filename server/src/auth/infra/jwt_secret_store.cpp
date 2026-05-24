#include "nebula/auth/infra/jwt_secret_store.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nebula/common/codec/base64.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/posix.hpp"

namespace nebula::auth {

namespace {

constexpr mode_t kJwtSecretForbiddenGroupOtherMask = 0077;
constexpr std::size_t kJwtSecretMinBytes = 32U;
constexpr std::size_t kJwtSecretMaxBytes = 4096U;

struct GeneratedJwtSecret {
    std::vector<std::byte> secret;
    std::string persisted_secret_base64;
};

void log_jwt_secret_too_large(const std::filesystem::path& path, std::uintmax_t count) {
    common::Logger::instance()
        .fatal("load jwt secret failed")
        .field("path", path)
        .field("error", "secret_too_large")
        .field("count", count)
        .field("max_secret_bytes", kJwtSecretMaxBytes)
        .field("decision", "exit_process");
}

std::expected<std::string, JwtSecretStoreError> read_jwt_secret_content(int secret_fd,
                                                                        const std::filesystem::path& path) {
    std::string encoded_secret;
    std::array<char, 4096> buffer{};
    while (true) {
        errno = 0;
        const ssize_t read_n = ::read(secret_fd, buffer.data(), buffer.size());
        if (read_n > 0) {
            const auto chunk_size = static_cast<std::size_t>(read_n);
            if (encoded_secret.size() > kJwtSecretMaxBytes) {
                log_jwt_secret_too_large(path, static_cast<std::uintmax_t>(encoded_secret.size()));
                return std::unexpected(JwtSecretStoreError::SecretTooLarge);
            }
            const std::size_t remaining = kJwtSecretMaxBytes - encoded_secret.size();
            if (chunk_size > remaining) {
                log_jwt_secret_too_large(path, static_cast<std::uintmax_t>(encoded_secret.size()) + chunk_size);
                return std::unexpected(JwtSecretStoreError::SecretTooLarge);
            }
            encoded_secret.append(buffer.data(), chunk_size);
            continue;
        }
        if (read_n == 0) {
            return encoded_secret;
        }
        if (errno == EINTR) {
            continue;
        }

        const int err = errno;
        common::Logger::instance()
            .fatal("read jwt secret failed")
            .field("path", path)
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::ReadFailed);
    }
}

std::expected<std::vector<std::byte>, JwtSecretStoreError> read_jwt_secret_file(const std::filesystem::path& path,
                                                                                bool log_not_found) {
    int open_flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif

    errno = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int secret_fd = ::open(path.c_str(), open_flags);
    if (secret_fd < 0) {
        const int err = errno;
        if (err == ENOENT) {
            if (log_not_found) {
                common::Logger::instance()
                    .fatal("load jwt secret failed")
                    .field("path", path)
                    .field("errno", err)
                    .field("error", common::errno_message(err))
                    .field("decision", "exit_process");
            }
            return std::unexpected(JwtSecretStoreError::NotFound);
        }
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::OpenFailed);
    }

    struct stat file_stat{};
    errno = 0;
    if (::fstat(secret_fd, &file_stat) != 0) {
        const int err = errno;
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal("stat jwt secret failed")
            .field("path", path)
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::StatFailed);
    }

    if (!S_ISREG(file_stat.st_mode)) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "not_regular_file")
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::NotRegularFile);
    }

    const mode_t file_mode = file_stat.st_mode & 0777;
    if ((file_mode & kJwtSecretForbiddenGroupOtherMask) != 0) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "insecure_permissions")
            .field("file_mode", std::format("{:04o}", static_cast<unsigned int>(file_mode)))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::InsecurePermissions);
    }

    if (file_stat.st_size < 0) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "invalid_file_size")
            .field("count", static_cast<std::intmax_t>(file_stat.st_size))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::InvalidFileSize);
    }

    const auto file_size = static_cast<std::uintmax_t>(file_stat.st_size);
    if (std::cmp_greater(file_size, kJwtSecretMaxBytes)) {
        common::close_fd(secret_fd);
        log_jwt_secret_too_large(path, file_size);
        return std::unexpected(JwtSecretStoreError::SecretTooLarge);
    }

    auto encoded_secret = read_jwt_secret_content(secret_fd, path);
    common::close_fd(secret_fd);
    if (!encoded_secret.has_value()) {
        return std::unexpected(encoded_secret.error());
    }

    if (encoded_secret->empty()) {
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "empty_value")
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::EmptyValue);
    }

    auto decoded_secret = common::base64_decode_to_bytes(*encoded_secret);
    if (!decoded_secret.has_value()) {
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "invalid_secret_encoding")
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::InvalidSecretEncoding);
    }

    if (decoded_secret->size() < kJwtSecretMinBytes) {
        common::Logger::instance()
            .fatal("load jwt secret failed")
            .field("path", path)
            .field("error", "weak_value")
            .field("min_secret_bytes", kJwtSecretMinBytes)
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::WeakValue);
    }

    return std::move(*decoded_secret);
}

std::string openssl_error_message() {
    const auto err = ::ERR_get_error();
    if (err == 0UL) {
        return "openssl_error_unknown";
    }

    std::array<char, 256> buffer{};
    ::ERR_error_string_n(err, buffer.data(), buffer.size());
    return {buffer.data()};
}

std::optional<GeneratedJwtSecret> generate_jwt_secret_value(const std::filesystem::path& path) {
    std::array<std::uint8_t, kJwtSecretMinBytes> random_bytes{};
    if (::RAND_priv_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        const std::string openssl_error = openssl_error_message();
        common::Logger::instance()
            .fatal("generate jwt secret failed")
            .field("path", path)
            .field("error", "openssl_rand_failed")
            .field("error_detail", openssl_error)
            .field("decision", "exit_process");
        return std::nullopt;
    }

    GeneratedJwtSecret generated;
    generated.secret.reserve(random_bytes.size());
    for (const std::uint8_t byte : random_bytes) {
        generated.secret.push_back(static_cast<std::byte>(byte));
    }
    generated.persisted_secret_base64 = common::base64_encode(std::as_bytes(std::span{random_bytes}));
    return generated;
}

std::expected<bool, JwtSecretStoreError> persist_jwt_secret_file(const std::filesystem::path& path,
                                                                 std::string_view secret) {
    const std::filesystem::path secret_dir = path.parent_path();
    if (!secret_dir.empty()) {
        std::error_code create_dir_error;
        std::filesystem::create_directories(secret_dir, create_dir_error);
        if (create_dir_error) {
            common::Logger::instance()
                .fatal("create jwt secret dir failed")
                .field("path", secret_dir)
                .field("errno", create_dir_error.value())
                .field("error", create_dir_error.message())
                .field("decision", "exit_process");
            return std::unexpected(JwtSecretStoreError::CreateDirectoryFailed);
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int secret_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (secret_fd < 0) {
        const int err = errno;
        if (err == EEXIST) {
            return false;
        }
        common::Logger::instance()
            .fatal("write jwt secret failed")
            .field("path", path)
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::WriteFailed);
    }

    std::span<const char> remaining(secret.data(), secret.size());
    while (!remaining.empty()) {
        errno = 0;
        const ssize_t write_n = ::write(secret_fd, remaining.data(), remaining.size());
        if (write_n > 0) {
            remaining = remaining.subspan(static_cast<std::size_t>(write_n));
            continue;
        }
        if (write_n < 0 && errno == EINTR) {
            continue;
        }

        const int err = write_n == 0 ? 0 : errno;
        common::close_fd(secret_fd);
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        common::Logger::instance()
            .fatal("write jwt secret failed")
            .field("path", path)
            .field("errno", err)
            .field("error", common::errno_message(err))
            .field("decision", "exit_process");
        return std::unexpected(JwtSecretStoreError::WriteFailed);
    }
    common::close_fd(secret_fd);

    common::Logger::instance().info("jwt secret generated").field("path", path);
    return true;
}

}  // namespace

std::string_view to_string(JwtSecretStoreError error) noexcept {
    switch (error) {
        case JwtSecretStoreError::NotFound:
            return "not_found";
        case JwtSecretStoreError::OpenFailed:
            return "open_failed";
        case JwtSecretStoreError::StatFailed:
            return "stat_failed";
        case JwtSecretStoreError::NotRegularFile:
            return "not_regular_file";
        case JwtSecretStoreError::InsecurePermissions:
            return "insecure_permissions";
        case JwtSecretStoreError::InvalidFileSize:
            return "invalid_file_size";
        case JwtSecretStoreError::SecretTooLarge:
            return "secret_too_large";
        case JwtSecretStoreError::ReadFailed:
            return "read_failed";
        case JwtSecretStoreError::EmptyValue:
            return "empty_value";
        case JwtSecretStoreError::InvalidSecretEncoding:
            return "invalid_secret_encoding";
        case JwtSecretStoreError::WeakValue:
            return "weak_value";
        case JwtSecretStoreError::CreateDirectoryFailed:
            return "create_directory_failed";
        case JwtSecretStoreError::GenerateFailed:
            return "generate_failed";
        case JwtSecretStoreError::WriteFailed:
            return "write_failed";
    }
    std::unreachable();
}

std::expected<std::vector<std::byte>, JwtSecretStoreError> load_or_create_jwt_secret(
    const std::filesystem::path& path) {
    auto read_result = read_jwt_secret_file(path, false);
    if (read_result.has_value()) {
        return std::move(*read_result);
    }
    if (read_result.error() != JwtSecretStoreError::NotFound) {
        return std::unexpected(read_result.error());
    }

    std::optional<GeneratedJwtSecret> generated = generate_jwt_secret_value(path);
    if (!generated.has_value()) {
        return std::unexpected(JwtSecretStoreError::GenerateFailed);
    }

    const auto persist_result = persist_jwt_secret_file(path, generated->persisted_secret_base64);
    if (!persist_result.has_value()) {
        return std::unexpected(persist_result.error());
    }
    if (*persist_result) {
        return std::move(generated->secret);
    }

    read_result = read_jwt_secret_file(path, true);
    if (!read_result.has_value()) {
        return std::unexpected(read_result.error());
    }
    return std::move(*read_result);
}

}  // namespace nebula::auth
