#pragma once

#include "shclog/types.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#include <concepts>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
namespace shclog {

template <typename T>
concept an_integer =
    // in_range doesnt work for char conversions in C++ because yes
    std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
    !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
    !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

// Deferred placeholder, never actually used
struct deferred_t {};

// returned after deferred type is used in order to use a cast later when
// <To> is qualified
template <an_integer From> struct [[nodiscard]] int_cast_proxy {
    From v;

    // int<->int in range (does not truncate)
    // important to notice that the template for To is evaluated on call
    template <an_integer To> constexpr operator To() const noexcept {
        assert(std::in_range<To>(v));
        return static_cast<To>(v);
    }
};

// int<->int
template <typename To = deferred_t, an_integer From>
[[nodiscard]] constexpr auto int_cast(const From v) noexcept {
    // deferred branch, deduce_t is used when To can't be guessed
    // we return int_cast_proxy, which is later translated to <To> To()
    // to be able to cast to the target type, example:
    // u8 x = int_cast(10)
    if constexpr (std::same_as<To, deferred_t>)
        return int_cast_proxy<From>{v};
    // no-op
    else if constexpr (std::same_as<To, From>)
        return v;
    // <To> is not deferred, example int_cast<u8>(10)
    else {
        assert(std::in_range<To>(v));
        return static_cast<To>(v);
    }
}

// float->int, notice floating_point doesnt need the same integers shenanigan
// for concept definition because they dont mix with special 'chars' or bool
template <an_integer To, std::floating_point From>
[[nodiscard]] constexpr To int_cast(const From v) noexcept {
    // i32, u32 digis = 31, 32
    // f32, f64 max_exponent = 128, 1024
    // so this guard is mostly for f16 or high integer types if later
    // supported
    constexpr auto bits = std::numeric_limits<To>::digits;
    if constexpr (bits < std::numeric_limits<From>::max_exponent) {
        // first digit in which we cant fit significant bits inside To
        // keep in mind floats > 2^<f64>::digits (53) are lossy at source, so we
        // dont care
        // std::ldexp is not constexpr accordign to clang-tidy and since we are
        // handling a float we cant use * 2
        [[maybe_unused]] constexpr From hi =
            static_cast<From>((std::numeric_limits<To>::max() >> 1) + 1) * 2;
        if constexpr (std::is_signed_v<To>)
            assert(v >= -hi && v < hi);
        else
            // floats are always signed, so -1 is the first bad value
            assert(v > From{-1} && v < hi);
    } else {
        // every finite From value fits in To; reject inf/NaN and sign only
        if constexpr (std::is_signed_v<To>)
            assert(std::isfinite(v));
        else
            assert(std::isfinite(v) && v > From{-1});
    }
    return static_cast<To>(v);
}

template <std::floating_point To, an_integer From>
[[nodiscard]] constexpr To float_cast(const From v) noexcept {
    // this is lossy
    return static_cast<To>(v);
}

template <std::floating_point To, std::floating_point From>
[[nodiscard]] constexpr To float_cast(const From v) noexcept {
    // Narrowing range check, otherwise promotion just works
    if constexpr (std::numeric_limits<To>::max_exponent <
                  std::numeric_limits<From>::max_exponent) {
        [[maybe_unused]] constexpr auto max =
            static_cast<From>(std::numeric_limits<To>::max());
        assert(!std::isfinite(v) || (v >= -max && v <= max));
    }
    return static_cast<To>(v);
}

// alignment is implicit in T
template <typename T> [[nodiscard]] T *ptr_from_int(const usize n) noexcept {
    assert(n != 0);
    assert(n % alignof(T) == 0);
    // not using reinterpret_cast because tidy doesnt like it
    // here, even though we are only working with equilavent containers
    return std::bit_cast<T *>(n);
}

// alignment is implicit in T
template <typename T, usize N>
[[nodiscard]] T *align_cast(T *const p) noexcept {
    assert(std::bit_cast<usize>(p) % N == 0);
    return std::assume_aligned<N>(p);
}

template <typename To, typename From>
[[nodiscard]] To *ptr_cast(From *const p) noexcept {
    // by not moving from ptr->int then int->ptr we dont lose
    // provenance, but the -Wcast-align hates it
    // even though we check it here
    assert(std::bit_cast<usize>(p) % alignof(To) == 0);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
    return reinterpret_cast<To *>(p);
#pragma GCC diagnostic pop
}
} // namespace shclog
