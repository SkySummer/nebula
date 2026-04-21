#include "nebula/auth/jwt_secret_store.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nebula/common/base64.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"

namespace nebula::auth {

namespace {

enum class JwtSecretReadResult : std::uint8_t {
    Loaded,
    NotFound,
    Failed,
};

enum class JwtSecretPersistResult : std::uint8_t {
    Persisted,
    AlreadyExists,
    Failed,
};

constexpr mode_t kJwtSecretForbiddenGroupOtherMask = 0077;
constexpr std::size_t kJwtSecretMinBytes = 32U;
constexpr std::size_t kJwtSecretMaxBytes = 4096U;

struct GeneratedJwtSecret {
    std::string secret;
    std::string persisted_secret_base64;
};

std::string bytes_to_secret(std::span<const std::uint8_t> bytes) {
    std::string secret;
    secret.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        secret.push_back(static_cast<char>(byte));
    }
    return secret;
}

void log_jwt_secret_too_large(const std::filesystem::path& path, std::uintmax_t count) {
    common::Logger::instance()
        .fatal(common::LogDomain::Auth, "load jwt secret failed")
        .field("path", path.string())
        .field("error", "secret_too_large")
        .field("count", count)
        .field("max_secret_bytes", kJwtSecretMaxBytes)
        .field("decision", "exit_process");
}

JwtSecretReadResult read_jwt_secret_content(int secret_fd, const std::filesystem::path& path, std::string& secret_out) {
    secret_out.clear();
    std::array<char, 4096> buffer{};
    while (true) {
        errno = 0;
        const ssize_t read_n = ::read(secret_fd, buffer.data(), buffer.size());
        if (read_n > 0) {
            const auto chunk_size = static_cast<std::size_t>(read_n);
            if (secret_out.size() > kJwtSecretMaxBytes) {
                log_jwt_secret_too_large(path, static_cast<std::uintmax_t>(secret_out.size()));
                return JwtSecretReadResult::Failed;
            }
            const std::size_t remaining = kJwtSecretMaxBytes - secret_out.size();
            if (chunk_size > remaining) {
                log_jwt_secret_too_large(path, static_cast<std::uintmax_t>(secret_out.size()) + chunk_size);
                return JwtSecretReadResult::Failed;
            }
            secret_out.append(buffer.data(), chunk_size);
            continue;
        }
        if (read_n == 0) {
            return JwtSecretReadResult::Loaded;
        }
        if (errno == EINTR) {
            continue;
        }

        const int err = errno;
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "read jwt secret failed")
            .field("path", path.string())
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }
}

JwtSecretReadResult read_jwt_secret_file(const std::filesystem::path& path, bool log_not_found,
                                         std::string& secret_out) {
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
                    .fatal(common::LogDomain::Auth, "load jwt secret failed")
                    .field("path", path.string())
                    .field("errno", err, common::errno_message(err))
                    .field("decision", "exit_process");
            }
            return JwtSecretReadResult::NotFound;
        }
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    struct stat file_stat{};
    errno = 0;
    if (::fstat(secret_fd, &file_stat) != 0) {
        const int err = errno;
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "stat jwt secret failed")
            .field("path", path.string())
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    if (!S_ISREG(file_stat.st_mode)) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "not_regular_file")
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    const mode_t file_mode = file_stat.st_mode & 0777;
    if ((file_mode & kJwtSecretForbiddenGroupOtherMask) != 0) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "insecure_permissions")
            .field("file_mode", std::format("{:04o}", static_cast<unsigned int>(file_mode)))
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    if (file_stat.st_size < 0) {
        common::close_fd(secret_fd);
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "invalid_file_size")
            .field("count", static_cast<std::intmax_t>(file_stat.st_size))
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    const auto file_size = static_cast<std::uintmax_t>(file_stat.st_size);
    if (file_size > static_cast<std::uintmax_t>(kJwtSecretMaxBytes)) {
        common::close_fd(secret_fd);
        log_jwt_secret_too_large(path, file_size);
        return JwtSecretReadResult::Failed;
    }

    const JwtSecretReadResult read_result = read_jwt_secret_content(secret_fd, path, secret_out);
    common::close_fd(secret_fd);
    if (read_result != JwtSecretReadResult::Loaded) {
        return read_result;
    }

    if (secret_out.empty()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "empty_value")
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    std::optional<std::vector<std::uint8_t>> decoded_secret = common::base64_decode_to_bytes(secret_out);
    if (!decoded_secret.has_value()) {
        decoded_secret = common::base64url_decode_to_bytes(secret_out);
    }
    if (!decoded_secret.has_value()) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "invalid_secret_encoding")
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    if (decoded_secret->size() < kJwtSecretMinBytes) {
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "load jwt secret failed")
            .field("path", path.string())
            .field("error", "weak_value")
            .field("min_secret_bytes", kJwtSecretMinBytes)
            .field("decision", "exit_process");
        return JwtSecretReadResult::Failed;
    }

    secret_out = bytes_to_secret(std::span<const std::uint8_t>(decoded_secret->data(), decoded_secret->size()));
    return JwtSecretReadResult::Loaded;
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
    std::array<std::uint8_t, 32> random_bytes{};
    if (::RAND_priv_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        const std::string openssl_error = openssl_error_message();
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "generate jwt secret failed")
            .field("path", path.string())
            .field("error", "openssl_rand_failed", openssl_error)
            .field("decision", "exit_process");
        return std::nullopt;
    }

    GeneratedJwtSecret generated;
    const std::span<const std::uint8_t> secret_bytes(random_bytes.data(), random_bytes.size());
    generated.secret = bytes_to_secret(secret_bytes);
    generated.persisted_secret_base64 = common::base64_encode(secret_bytes);
    return generated;
}

JwtSecretPersistResult persist_jwt_secret_file(const std::filesystem::path& path, std::string_view secret) {
    const std::filesystem::path secret_dir = path.parent_path();
    if (!secret_dir.empty()) {
        std::error_code create_dir_error;
        std::filesystem::create_directories(secret_dir, create_dir_error);
        if (create_dir_error) {
            common::Logger::instance()
                .fatal(common::LogDomain::Auth, "create jwt secret dir failed")
                .field("path", secret_dir.string())
                .field("errno", create_dir_error.value(), create_dir_error.message())
                .field("decision", "exit_process");
            return JwtSecretPersistResult::Failed;
        }
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int secret_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (secret_fd < 0) {
        const int err = errno;
        if (err == EEXIST) {
            return JwtSecretPersistResult::AlreadyExists;
        }
        common::Logger::instance()
            .fatal(common::LogDomain::Auth, "write jwt secret failed")
            .field("path", path.string())
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_process");
        return JwtSecretPersistResult::Failed;
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
            .fatal(common::LogDomain::Auth, "write jwt secret failed")
            .field("path", path.string())
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_process");
        return JwtSecretPersistResult::Failed;
    }
    common::close_fd(secret_fd);

    common::Logger::instance().info(common::LogDomain::Auth, "jwt secret generated").field("path", path.string());
    return JwtSecretPersistResult::Persisted;
}

}  // namespace

std::optional<std::string> load_or_create_jwt_secret(const std::filesystem::path& path) {
    std::string secret;

    const JwtSecretReadResult read_result = read_jwt_secret_file(path, false, secret);
    switch (read_result) {
        case JwtSecretReadResult::Loaded:
            return secret;
        case JwtSecretReadResult::NotFound:
            break;
        case JwtSecretReadResult::Failed:
            return std::nullopt;
    }

    std::optional<GeneratedJwtSecret> generated = generate_jwt_secret_value(path);
    if (!generated.has_value()) {
        return std::nullopt;
    }

    const JwtSecretPersistResult persist_result = persist_jwt_secret_file(path, generated->persisted_secret_base64);
    switch (persist_result) {
        case JwtSecretPersistResult::Persisted:
            return generated->secret;
        case JwtSecretPersistResult::AlreadyExists:
            break;
        case JwtSecretPersistResult::Failed:
            return std::nullopt;
    }

    secret.clear();
    if (read_jwt_secret_file(path, true, secret) != JwtSecretReadResult::Loaded) {
        return std::nullopt;
    }
    return secret;
}

}  // namespace nebula::auth
