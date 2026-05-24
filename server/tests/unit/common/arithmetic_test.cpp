#include "nebula/common/base/arithmetic.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_saturating_add_int64_returns_sum_without_overflow() {
    bool saturated = false;
    const auto value = common::saturating_add<std::int64_t>(7, 9, saturated);
    test::expect_equal(value, std::int64_t{16}, "int64 add should return exact sum");
    test::expect_true(!saturated, "int64 add should not mark saturation on exact sum");
}

void test_saturating_add_int64_saturates_to_max() {
    bool saturated = false;
    const auto value =
        common::saturating_add<std::int64_t>(std::numeric_limits<std::int64_t>::max(), std::int64_t{1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::max(), "int64 add overflow should clamp to max");
    test::expect_true(saturated, "int64 add overflow should mark saturation");
}

void test_saturating_add_int64_saturates_to_min() {
    bool saturated = false;
    const auto value =
        common::saturating_add<std::int64_t>(std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::min(), "int64 add underflow should clamp to min");
    test::expect_true(saturated, "int64 add underflow should mark saturation");
}

void test_saturating_sub_int64_saturates_to_max() {
    bool saturated = false;
    const auto value =
        common::saturating_sub<std::int64_t>(std::numeric_limits<std::int64_t>::max(), std::int64_t{-1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::max(), "int64 sub overflow should clamp to max");
    test::expect_true(saturated, "int64 sub overflow should mark saturation");
}

void test_saturating_sub_int64_saturates_to_min() {
    bool saturated = false;
    const auto value =
        common::saturating_sub<std::int64_t>(std::numeric_limits<std::int64_t>::min(), std::int64_t{1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::min(), "int64 sub underflow should clamp to min");
    test::expect_true(saturated, "int64 sub underflow should mark saturation");
}

void test_saturating_mul_int64_saturates_to_max() {
    bool saturated = false;
    const auto value = common::saturating_mul<std::int64_t>((std::numeric_limits<std::int64_t>::max() / 2) + 1,
                                                            std::int64_t{2}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::max(), "int64 mul overflow should clamp to max");
    test::expect_true(saturated, "int64 mul overflow should mark saturation");
}

void test_saturating_mul_int64_saturates_to_min() {
    bool saturated = false;
    const auto value =
        common::saturating_mul<std::int64_t>(std::numeric_limits<std::int64_t>::min(), std::int64_t{2}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::min(), "int64 mul underflow should clamp to min");
    test::expect_true(saturated, "int64 mul underflow should mark saturation");
}

void test_saturating_mul_int64_min_times_minus_one_saturates_to_max() {
    bool saturated = false;
    const auto value =
        common::saturating_mul<std::int64_t>(std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::int64_t>::max(),
                       "int64 min times minus one should clamp to max");
    test::expect_true(saturated, "int64 min times minus one should mark saturation");
}

void test_saturating_add_size_t_saturates_to_max() {
    bool saturated = false;
    const auto value =
        common::saturating_add<std::size_t>(std::numeric_limits<std::size_t>::max(), std::size_t{1}, saturated);
    test::expect_equal(value, std::numeric_limits<std::size_t>::max(), "size_t add overflow should clamp to max");
    test::expect_true(saturated, "size_t add overflow should mark saturation");
}

void test_saturating_sub_size_t_saturates_to_zero() {
    bool saturated = false;
    const auto value = common::saturating_sub<std::size_t>(std::size_t{3}, std::size_t{7}, saturated);
    test::expect_equal(value, std::size_t{0}, "size_t sub underflow should clamp to zero");
    test::expect_true(saturated, "size_t sub underflow should mark saturation");
}

void test_saturating_mul_size_t_saturates_to_max() {
    bool saturated = false;
    const auto value = common::saturating_mul<std::size_t>((std::numeric_limits<std::size_t>::max() / 2) + 1,
                                                           std::size_t{2}, saturated);
    test::expect_equal(value, std::numeric_limits<std::size_t>::max(), "size_t mul overflow should clamp to max");
    test::expect_true(saturated, "size_t mul overflow should mark saturation");
}

int run_arithmetic_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"saturating add int64 returns sum without overflow", test_saturating_add_int64_returns_sum_without_overflow},
        {"saturating add int64 saturates to max", test_saturating_add_int64_saturates_to_max},
        {"saturating add int64 saturates to min", test_saturating_add_int64_saturates_to_min},
        {"saturating sub int64 saturates to max", test_saturating_sub_int64_saturates_to_max},
        {"saturating sub int64 saturates to min", test_saturating_sub_int64_saturates_to_min},
        {"saturating mul int64 saturates to max", test_saturating_mul_int64_saturates_to_max},
        {"saturating mul int64 saturates to min", test_saturating_mul_int64_saturates_to_min},
        {"saturating mul int64 min times minus one saturates to max",
         test_saturating_mul_int64_min_times_minus_one_saturates_to_max},
        {"saturating add size_t saturates to max", test_saturating_add_size_t_saturates_to_max},
        {"saturating sub size_t saturates to zero", test_saturating_sub_size_t_saturates_to_zero},
        {"saturating mul size_t saturates to max", test_saturating_mul_size_t_saturates_to_max},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_arithmetic_tests);
}
