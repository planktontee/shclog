#pragma once

#include "shclog/cast.hpp"
#include <cmath>
#include <type_traits>

namespace shclog::math {

template <class T>
concept Arithmetic = an_integer<T> || std::floating_point<T>;

template <Arithmetic T> struct Overflow {
    T value;
    bool overflow;
};

template <Arithmetic T>
[[nodiscard]] inline constexpr Overflow<T>
mul_with_overflow(const T a, const T b) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
        const T v = a * b;
        return {v, !std::isfinite(v) && std::isfinite(a) && std::isfinite(b)};
    } else {

        T v;
        const bool o = __builtin_mul_overflow(a, b, &v);
        return {v, o};
    }
}

template <Arithmetic T>
[[nodiscard]] inline constexpr Overflow<T>
add_with_overflow(const T a, const T b) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
        const T v = a + b;
        return {v, !std::isfinite(v) && std::isfinite(a) && std::isfinite(b)};
    } else {
        T v;

        const bool o = __builtin_add_overflow(a, b, &v);
        return {v, o};
    }
}

template <typename T, typename E> constexpr T ipow(T base, E exp) noexcept {
    T result{1};
    while (exp) {
        if (exp & 1)
            result *= base;
        exp >>= 1;
        if (exp)
            base *= base;
    }
    return result;
}
} // namespace shclog::math
