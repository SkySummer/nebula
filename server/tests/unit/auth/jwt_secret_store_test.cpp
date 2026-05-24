#include "nebula/auth/infra/jwt_secret_store.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

#include "nebula/common/codec/base64.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_load_or_create_generates_secret_with_mode_0600() {
    const test::TempDir dir("nebula-jwt-secret-store-generate");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    if (!secret.has_value()) {
        nebula::test::fail("jwt secret should be generated");
    }

    test::expect_true(std::filesystem::exists(secret_path), "jwt secret file should exist");

    struct stat secret_stat{};
    test::expect_true(::stat(secret_path.c_str(), &secret_stat) == 0, "jwt secret file should be stat-able");
    test::expect_true(S_ISREG(secret_stat.st_mode), "jwt secret path should be regular file");
    test::expect_equal(secret_stat.st_mode & 0777, mode_t{0600}, "jwt secret file mode should be 0600");

    const std::string persisted_secret = test::read_all(secret_path);
    const auto decoded_secret = nebula::common::base64_decode_to_bytes(persisted_secret);
    if (!decoded_secret.has_value()) {
        nebula::test::fail("persisted jwt secret should be base64");
    }
    test::expect_equal(*decoded_secret, *secret, "decoded persisted jwt secret should equal returned value");
}

void test_load_or_create_rejects_insecure_permissions() {
    const test::TempDir dir("nebula-jwt-secret-store-insecure-mode");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    test::write_file(secret_path, std::string(32U, 'a'));

    std::error_code permissions_error;
    std::filesystem::permissions(
        secret_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace, permissions_error);
    test::expect_true(!permissions_error, "jwt secret insecure permissions should be set");

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    test::expect_true(!secret.has_value(), "jwt secret with insecure mode should be rejected");
    test::expect_equal(secret.error(), auth::JwtSecretStoreError::InsecurePermissions,
                       "jwt secret insecure mode should return insecure permissions error");
}

void test_load_or_create_accepts_owner_read_only_permissions() {
    const test::TempDir dir("nebula-jwt-secret-store-owner-read-only");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    const std::string expected_secret = "owner_read_only_jwt_secret_0123456789";
    test::write_file(secret_path, nebula::common::base64_encode(expected_secret));
    test::set_owner_read_only(secret_path);

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    if (!secret.has_value()) {
        nebula::test::fail("jwt secret with owner-only read permission should be accepted");
    }
    test::expect_equal(*secret, test::to_byte_vector(expected_secret),
                       "jwt secret should match persisted content bytes");
}

void test_load_or_create_rejects_base64url_secret() {
    const test::TempDir dir("nebula-jwt-secret-store-base64url-rejected");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    const std::vector<std::byte> base64url_only_secret(32U, std::byte{0xFB});
    test::write_file(secret_path, nebula::common::base64url_encode(base64url_only_secret));
    test::set_owner_read_only(secret_path);

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    test::expect_true(!secret.has_value(), "base64url jwt secret should be rejected");
    test::expect_equal(secret.error(), auth::JwtSecretStoreError::InvalidSecretEncoding,
                       "base64url jwt secret should return invalid encoding error");
}

void test_load_or_create_preserves_binary_secret_bytes() {
    const test::TempDir dir("nebula-jwt-secret-store-binary-secret");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    const std::vector<std::byte> expected_secret = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}, std::byte{0x11},
        std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
        std::byte{0x88}, std::byte{0x99}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
        std::byte{0xEE}, std::byte{0xF0}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x81}, std::byte{0x92}, std::byte{0xA3},
        std::byte{0xB4}, std::byte{0xC5},
    };
    test::write_file(secret_path, nebula::common::base64_encode(expected_secret));
    test::set_owner_read_only(secret_path);

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    if (!secret.has_value()) {
        nebula::test::fail("binary jwt secret should be accepted");
    }
    test::expect_equal(*secret, expected_secret, "binary jwt secret bytes should round trip without string conversion");
}

void test_load_or_create_rejects_weak_secret() {
    const test::TempDir dir("nebula-jwt-secret-store-weak-secret");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    test::write_file(secret_path, nebula::common::base64_encode("a"));
    test::set_owner_read_write_only(secret_path);

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    test::expect_true(!secret.has_value(), "weak jwt secret should be rejected");
    test::expect_equal(secret.error(), auth::JwtSecretStoreError::WeakValue,
                       "weak jwt secret should return weak value error");
}

void test_load_or_create_rejects_oversized_secret() {
    const test::TempDir dir("nebula-jwt-secret-store-oversized-secret");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";
    test::write_file(secret_path, std::string(std::size_t{2} * 1024U * 1024U, 'a'));
    test::set_owner_read_write_only(secret_path);

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    test::expect_true(!secret.has_value(), "oversized jwt secret should be rejected");
    test::expect_equal(secret.error(), auth::JwtSecretStoreError::SecretTooLarge,
                       "oversized jwt secret should return too large error");
}

void test_load_or_create_rejects_non_regular_file() {
    const test::TempDir dir("nebula-jwt-secret-store-non-regular");
    const std::filesystem::path secret_path = dir.path() / "jwt.key";

    std::error_code create_dir_error;
    std::filesystem::create_directories(secret_path, create_dir_error);
    test::expect_true(!create_dir_error, "non regular jwt secret path should be created as directory");

    const auto secret = auth::load_or_create_jwt_secret(secret_path);
    test::expect_true(!secret.has_value(), "jwt secret directory path should be rejected");
    test::expect_equal(secret.error(), auth::JwtSecretStoreError::NotRegularFile,
                       "jwt secret directory path should return not regular file error");
}

int run_jwt_secret_store_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"load or create generates secret with mode 0600", test_load_or_create_generates_secret_with_mode_0600},
        {"load or create rejects insecure permissions", test_load_or_create_rejects_insecure_permissions},
        {"load or create accepts owner read only permissions", test_load_or_create_accepts_owner_read_only_permissions},
        {"load or create rejects base64url secret", test_load_or_create_rejects_base64url_secret},
        {"load or create preserves binary secret bytes", test_load_or_create_preserves_binary_secret_bytes},
        {"load or create rejects weak secret", test_load_or_create_rejects_weak_secret},
        {"load or create rejects oversized secret", test_load_or_create_rejects_oversized_secret},
        {"load or create rejects non regular file", test_load_or_create_rejects_non_regular_file},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_jwt_secret_store_tests);
}
