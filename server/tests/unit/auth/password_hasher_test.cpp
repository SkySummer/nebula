#include "nebula/auth/infra/password_hasher.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nebula/auth/domain/limits.hpp"
#include "nebula/common/codec/base64.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_constructor_rejects_iterations_below_minimum() {
    static_assert(auth::kMinPasswordHashIterations > 0U);
    try {
        [[maybe_unused]] const auth::PasswordHasher hasher(
            auth::PasswordHashConfig{.iterations = auth::kMinPasswordHashIterations - 1U});
        test::fail("constructor should reject iterations below minimum");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("password_hash_iterations_invalid"),
                           "constructor should report invalid iterations");
    }
}

void test_constructor_rejects_salt_bytes_below_minimum() {
    static_assert(auth::kMinPasswordHashSaltBytes > 0U);
    try {
        [[maybe_unused]] const auth::PasswordHasher hasher(
            auth::PasswordHashConfig{.salt_bytes = auth::kMinPasswordHashSaltBytes - 1U});
        test::fail("constructor should reject salt bytes below minimum");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("password_hash_salt_bytes_invalid"),
                           "constructor should report invalid salt bytes");
    }
}

void test_constructor_rejects_salt_bytes_above_maximum() {
    try {
        [[maybe_unused]] const auth::PasswordHasher hasher(
            auth::PasswordHashConfig{.salt_bytes = auth::kMaxPasswordHashSaltBytes + 1U});
        test::fail("constructor should reject oversized salt bytes");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("password_hash_salt_bytes_invalid"),
                           "constructor should report invalid salt bytes");
    }
}

void test_constructor_rejects_derived_key_bytes_above_maximum() {
    try {
        [[maybe_unused]] const auth::PasswordHasher hasher(
            auth::PasswordHashConfig{.derived_key_bytes = auth::kMaxPasswordHashDerivedKeyBytes + 1U});
        test::fail("constructor should reject oversized derived key bytes");
    } catch (const std::invalid_argument& e) {
        test::expect_equal(std::string_view(e.what()), std::string_view("password_hash_derived_key_bytes_invalid"),
                           "constructor should report invalid derived key bytes");
    }
}

void test_hash_password_accepts_minimum_iterations() {
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(auth::PasswordHashConfig{.iterations = auth::kMinPasswordHashIterations});
    const std::optional<auth::PasswordHashValue> hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        test::fail("hash_password should accept minimum iterations");
    }
    test::expect_true(auth::PasswordHasher::verify_password(password, *hash),
                      "verify_password should accept hash generated with minimum iterations");
}

void test_verify_password_rejects_iterations_below_minimum() {
    static_assert(auth::kMinPasswordHashIterations > 0U);
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(auth::PasswordHashConfig{.iterations = auth::kMinPasswordHashIterations});
    const std::optional<auth::PasswordHashValue> hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        test::fail("hash_password should produce hash");
    }

    auth::PasswordHashValue weak_iterations_hash = *hash;
    weak_iterations_hash.iterations = auth::kMinPasswordHashIterations - 1U;
    test::expect_true(!auth::PasswordHasher::verify_password(password, weak_iterations_hash),
                      "verify_password should reject iterations below minimum");
}

void test_verify_password_rejects_salt_below_minimum() {
    static_assert(auth::kMinPasswordHashSaltBytes > 0U);
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(auth::PasswordHashConfig{.iterations = auth::kMinPasswordHashIterations});
    const std::optional<auth::PasswordHashValue> hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        test::fail("hash_password should produce hash");
    }

    auth::PasswordHashValue undersized_salt_hash = *hash;
    undersized_salt_hash.salt =
        nebula::common::base64url_encode(std::vector<std::byte>(auth::kMinPasswordHashSaltBytes - 1U, std::byte{9}));
    test::expect_true(!auth::PasswordHasher::verify_password(password, undersized_salt_hash),
                      "verify_password should reject salt length below allowed min");
}

void test_verify_password_rejects_salt_above_maximum() {
    const std::string password = "strong_password_123";
    const auth::PasswordHasher hasher(auth::PasswordHashConfig{.iterations = auth::kMinPasswordHashIterations});
    const std::optional<auth::PasswordHashValue> hash = hasher.hash_password(password);
    if (!hash.has_value()) {
        test::fail("hash_password should produce hash");
    }

    auth::PasswordHashValue oversized_salt_hash = *hash;
    oversized_salt_hash.salt =
        nebula::common::base64url_encode(std::vector<std::byte>(auth::kMaxPasswordHashSaltBytes + 1U, std::byte{9}));
    test::expect_true(!auth::PasswordHasher::verify_password(password, oversized_salt_hash),
                      "verify_password should reject salt length above allowed max");
}

int run_password_hasher_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"constructor rejects iterations below minimum", test_constructor_rejects_iterations_below_minimum},
        {"constructor rejects salt bytes below minimum", test_constructor_rejects_salt_bytes_below_minimum},
        {"constructor rejects salt bytes above maximum", test_constructor_rejects_salt_bytes_above_maximum},
        {"constructor rejects derived key bytes above maximum",
         test_constructor_rejects_derived_key_bytes_above_maximum},
        {"hash password accepts minimum iterations", test_hash_password_accepts_minimum_iterations},
        {"verify password rejects iterations below minimum", test_verify_password_rejects_iterations_below_minimum},
        {"verify password rejects salt below minimum", test_verify_password_rejects_salt_below_minimum},
        {"verify password rejects salt above maximum", test_verify_password_rejects_salt_above_maximum},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_password_hasher_tests);
}
