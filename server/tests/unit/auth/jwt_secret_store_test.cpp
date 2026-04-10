#include "nebula/auth/jwt_secret_store.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

#include "nebula/common/base64.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::auth::load_or_create_jwt_secret;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::read_all;
using nebula::testsupport::set_owner_read_only;
using nebula::testsupport::set_owner_read_write_only;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_file;

std::vector<std::uint8_t> as_bytes(std::string_view value) {
    std::vector<std::uint8_t> out;
    out.reserve(value.size());
    for (const char ch : value) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
    }
    return out;
}

void test_load_or_create_generates_secret_with_mode_0600() {
    const TempDir dir("nebula-jwt-secret-store-generate");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(secret.has_value(), "jwt secret should be generated");
    if (!secret.has_value()) {
        nebula::testsupport::fail("jwt secret should be generated");
    }

    expect_true(std::filesystem::exists(secret_path), "jwt secret file should exist");

    struct stat secret_stat{};
    expect_true(::stat(secret_path.c_str(), &secret_stat) == 0, "jwt secret file should be stat-able");
    expect_true(S_ISREG(secret_stat.st_mode), "jwt secret path should be regular file");
    expect_equal(secret_stat.st_mode & 0777, static_cast<mode_t>(0600), "jwt secret file mode should be 0600");

    const std::string persisted_secret = read_all(secret_path);
    const std::optional<std::vector<std::uint8_t>> decoded_secret =
        nebula::common::base64_decode_to_bytes(persisted_secret);
    expect_true(decoded_secret.has_value(), "persisted jwt secret should be base64");
    if (!decoded_secret.has_value()) {
        nebula::testsupport::fail("persisted jwt secret should be base64");
    }

    expect_equal(*decoded_secret, as_bytes(*secret), "decoded persisted jwt secret should equal returned value");
}

void test_load_or_create_rejects_insecure_permissions() {
    const TempDir dir("nebula-jwt-secret-store-insecure-mode");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    write_file(secret_path, std::string(32U, 'a'));

    std::error_code permissions_error;
    std::filesystem::permissions(
        secret_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace, permissions_error);
    expect_true(!permissions_error, "jwt secret insecure permissions should be set");

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(!secret.has_value(), "jwt secret with insecure mode should be rejected");
}

void test_load_or_create_accepts_owner_read_only_permissions() {
    const TempDir dir("nebula-jwt-secret-store-owner-read-only");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    const std::string expected_secret = "owner_read_only_jwt_secret_0123456789";
    write_file(secret_path, nebula::common::base64_encode(expected_secret));
    set_owner_read_only(secret_path);

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(secret.has_value(), "jwt secret with owner-only read permission should be accepted");
    if (!secret.has_value()) {
        nebula::testsupport::fail("jwt secret with owner-only read permission should be accepted");
    }

    expect_equal(*secret, expected_secret, "jwt secret should match persisted content");
}

void test_load_or_create_accepts_base64url_secret_for_backward_compatibility() {
    const TempDir dir("nebula-jwt-secret-store-base64url-backward-compatible");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    const std::string expected_secret = "base64url_compat_jwt_secret_0123456789";
    write_file(secret_path, nebula::common::base64url_encode(expected_secret));
    set_owner_read_only(secret_path);

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(secret.has_value(), "base64url jwt secret should be accepted for backward compatibility");
    if (!secret.has_value()) {
        nebula::testsupport::fail("base64url jwt secret should be accepted for backward compatibility");
    }

    expect_equal(*secret, expected_secret, "decoded base64url jwt secret should match expected content");
}

void test_load_or_create_rejects_weak_secret() {
    const TempDir dir("nebula-jwt-secret-store-weak-secret");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    write_file(secret_path, nebula::common::base64_encode("a"));
    set_owner_read_write_only(secret_path);

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(!secret.has_value(), "weak jwt secret should be rejected");
}

void test_load_or_create_rejects_oversized_secret() {
    const TempDir dir("nebula-jwt-secret-store-oversized-secret");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    write_file(secret_path, std::string(static_cast<std::size_t>(2U) * 1024U * 1024U, 'a'));
    set_owner_read_write_only(secret_path);

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(!secret.has_value(), "oversized jwt secret should be rejected");
}

void test_load_or_create_rejects_non_regular_file() {
    const TempDir dir("nebula-jwt-secret-store-non-regular");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";

    std::error_code create_dir_error;
    std::filesystem::create_directories(secret_path, create_dir_error);
    expect_true(!create_dir_error, "non regular jwt secret path should be created as directory");

    const std::optional<std::string> secret = load_or_create_jwt_secret(secret_path);
    expect_true(!secret.has_value(), "jwt secret directory path should be rejected");
}

int run_jwt_secret_store_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"load or create generates secret with mode 0600", test_load_or_create_generates_secret_with_mode_0600},
        {"load or create rejects insecure permissions", test_load_or_create_rejects_insecure_permissions},
        {"load or create accepts owner read only permissions", test_load_or_create_accepts_owner_read_only_permissions},
        {"load or create accepts base64url secret for backward compatibility",
         test_load_or_create_accepts_base64url_secret_for_backward_compatibility},
        {"load or create rejects weak secret", test_load_or_create_rejects_weak_secret},
        {"load or create rejects oversized secret", test_load_or_create_rejects_oversized_secret},
        {"load or create rejects non regular file", test_load_or_create_rejects_non_regular_file},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_jwt_secret_store_tests);
}
