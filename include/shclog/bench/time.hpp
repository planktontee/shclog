#pragma once

#include "shclog/bench/sample.hpp"
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <type_traits>

namespace shclog::bench::time {

using namespace shclog::bench::sample;

template <class D> inline constexpr bool is_duration_v = false;

template <class R, class P>
inline constexpr bool is_duration_v<std::chrono::duration<R, P>> = true;

template <class D>
concept Duration = is_duration_v<D>;

template <class T>
concept Arithmetic = an_integer<T> || std::floating_point<T>;

template <Arithmetic T, Duration D> struct CastTransformFn {
    constexpr T operator()(typename D::rep n) const noexcept {
        if constexpr (std::is_floating_point_v<T>)
            return float_cast<T>(n);
        else
            return int_cast<T>(n);
    }
};

template <class F, class T, class D>
concept DurationTransform =
    Duration<D> && Arithmetic<T> && std::invocable<F &, typename D::rep> &&
    requires(F &f, typename D::rep n) {
        { f(n) } noexcept -> std::same_as<T>;
    };

template <class C>
concept Clock = std::chrono::is_clock_v<C>;

template <class F>
concept NothrowFn = requires(F &f) {
    { f() } noexcept;
};

template <Clock C = std::chrono::steady_clock,
          Duration D = std::chrono::nanoseconds, class T, NothrowFn Fn,
          DurationTransform<T, D> F = CastTransformFn<T, D>>
auto timed(Sample<T> &sample, Fn &&fn, F transform = {}) noexcept {
    const auto t0 = C::now();
    const auto record = [&]() noexcept {
        // TODO: for float we need to handle conversion manually to avoid
        // truncation
        const auto v =
            transform(std::chrono::duration_cast<D>(C::now() - t0).count());
        if (sample.push(v) != Sample<T>::PushResult::Success) [[unlikely]]
            std::abort();
    };

    if constexpr (std::is_void_v<std::invoke_result_t<Fn &>>) {
        fn();
        record();
    } else {
        auto r = fn();
        record();
        return r;
    }
}

} // namespace shclog::bench::time
