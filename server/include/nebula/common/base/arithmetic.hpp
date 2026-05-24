#ifndef NEBULA_COMMON_BASE_ARITHMETIC_HPP
#define NEBULA_COMMON_BASE_ARITHMETIC_HPP

#include <concepts>
#include <limits>

namespace nebula::common {

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_add(T lhs, T rhs, bool& saturated) noexcept {
    if constexpr (std::numeric_limits<T>::is_signed) {
        if (rhs > 0 && lhs > (std::numeric_limits<T>::max() - rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        if (rhs < 0 && lhs < (std::numeric_limits<T>::min() - rhs)) {
            saturated = true;
            return std::numeric_limits<T>::min();
        }
        return T{lhs + rhs};
    } else {
        if (lhs > (std::numeric_limits<T>::max() - rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        return T{lhs + rhs};
    }
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_add(T lhs, T rhs) noexcept {
    bool saturated = false;
    return saturating_add(lhs, rhs, saturated);
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_sub(T lhs, T rhs, bool& saturated) noexcept {
    if constexpr (std::numeric_limits<T>::is_signed) {
        if (rhs > 0 && lhs < (std::numeric_limits<T>::min() + rhs)) {
            saturated = true;
            return std::numeric_limits<T>::min();
        }
        if (rhs < 0 && lhs > (std::numeric_limits<T>::max() + rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        return T{lhs - rhs};
    } else {
        if (lhs < rhs) {
            saturated = true;
            return T{0};
        }
        return T{lhs - rhs};
    }
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_sub(T lhs, T rhs) noexcept {
    bool saturated = false;
    return saturating_sub(lhs, rhs, saturated);
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_mul(T lhs, T rhs, bool& saturated) noexcept {
    if (lhs == 0 || rhs == 0) {
        return T{0};
    }

    if constexpr (std::numeric_limits<T>::is_signed) {
        if (lhs > 0 && rhs > 0 && lhs > (std::numeric_limits<T>::max() / rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        if (lhs > 0 && rhs < 0 && rhs < (std::numeric_limits<T>::min() / lhs)) {
            saturated = true;
            return std::numeric_limits<T>::min();
        }
        if (lhs < 0 && rhs > 0 && lhs < (std::numeric_limits<T>::min() / rhs)) {
            saturated = true;
            return std::numeric_limits<T>::min();
        }
        if (lhs < 0 && rhs < 0 && lhs < (std::numeric_limits<T>::max() / rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        return T{lhs * rhs};
    } else {
        if (lhs > (std::numeric_limits<T>::max() / rhs)) {
            saturated = true;
            return std::numeric_limits<T>::max();
        }
        return T{lhs * rhs};
    }
}

template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
[[nodiscard]] constexpr T saturating_mul(T lhs, T rhs) noexcept {
    bool saturated = false;
    return saturating_mul(lhs, rhs, saturated);
}

}  // namespace nebula::common

#endif  // NEBULA_COMMON_BASE_ARITHMETIC_HPP
