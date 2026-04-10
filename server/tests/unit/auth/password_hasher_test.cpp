#include "nebula/auth/password_hasher.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/auth/password_hash_limits.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::auth::kMinPasswordHashIterations;
using nebula::auth::PasswordHashConfig;
using nebula::auth::PasswordHasher;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::fail;

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

void test_hash_password_rejects_iterations_below_minimum() {
    static_assert(kMinPasswordHashIterations > 0U);
    const PasswordHasher hasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations - 1U});
    expect_true(!hasher.hash_password("strong_password_123").has_value(),
                "hash_password should reject iterations below minimum");
}

void test_hash_password_accepts_minimum_iterations() {
    const std::string password = "strong_password_123";
    const PasswordHasher hasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations});
    const std::optional<std::string> hash = hasher.hash_password(password);
    expect_true(hash.has_value(), "hash_password should accept minimum iterations");
    if (!hash.has_value()) {
        fail("hash_password should accept minimum iterations");
    }
    expect_true(PasswordHasher::verify_password(password, *hash),
                "verify_password should accept hash generated with minimum iterations");
}

void test_verify_password_rejects_iterations_below_minimum() {
    static_assert(kMinPasswordHashIterations > 0U);
    const std::string password = "strong_password_123";
    const PasswordHasher hasher(PasswordHashConfig{.iterations = kMinPasswordHashIterations});
    const std::optional<std::string> hash = hasher.hash_password(password);
    expect_true(hash.has_value(), "hash_password should produce hash");
    if (!hash.has_value()) {
        fail("hash_password should produce hash");
    }

    const std::vector<std::string_view> parts = split(*hash, '$');
    expect_equal(parts.size(), static_cast<std::size_t>(4U), "encoded hash should have 4 parts");
    if (parts.size() != 4U) {
        fail("encoded hash should have 4 parts");
    }

    const std::string weak_iterations_hash =
        nebula::auth::format_password_hash(kMinPasswordHashIterations - 1U, parts[2], parts[3]);
    expect_true(!PasswordHasher::verify_password(password, weak_iterations_hash),
                "verify_password should reject iterations below minimum");
}

int run_password_hasher_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"hash password rejects iterations below minimum", test_hash_password_rejects_iterations_below_minimum},
        {"hash password accepts minimum iterations", test_hash_password_accepts_minimum_iterations},
        {"verify password rejects iterations below minimum", test_verify_password_rejects_iterations_below_minimum},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_password_hasher_tests);
}
