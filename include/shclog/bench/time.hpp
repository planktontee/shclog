#pragma once

#include "shclog/bench/sample.hpp"
#include "shclog/cast.hpp"
#include <chrono>
#include <concepts>
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

template <class C>
concept Clock = std::chrono::is_clock_v<C>;

template <Clock C, Duration D>
using Delta = std::common_type_t<typename C::duration, D>;

template <Arithmetic T, Clock C, Duration D, Duration TarD>
struct CastTransformFn;

template <Arithmetic T, Clock C, Duration D, Duration TarD>
    requires an_integer<T>
struct CastTransformFn<T, C, D, TarD> {
    inline constexpr T operator()(const Delta<C, D> n) const noexcept {
        return int_cast<T>(std::chrono::duration_cast<TarD>(n).count());
    }
};

template <Arithmetic T, Clock C, Duration D, Duration TarD>
    requires std::floating_point<T>
struct CastTransformFn<T, C, D, TarD> {
    using TargetDuration = std::chrono::duration<T, typename TarD::period>;
    inline constexpr T operator()(const Delta<C, D> n) const noexcept {
        return TargetDuration(n).count();
    }
};

template <class F, class T, class C, class D, class TarD>
concept DurationTransform =
    Arithmetic<T> && Clock<C> && Duration<D> && Duration<TarD> &&
    requires(F &f, const Delta<C, D> n) {
        { f(n) } noexcept -> std::same_as<T>;
    };

template <Clock C = std::chrono::steady_clock,
          Duration D = std::chrono::nanoseconds>
struct Time {

  public:
    void start() noexcept { t0 = C::now(); }

    [[nodiscard]] bool started() const noexcept { return t0 != SENTINEL; }

    template <class T, Duration TarD = D,
              DurationTransform<T, C, D, TarD> Transform =
                  CastTransformFn<T, C, D, TarD>>
    [[nodiscard]] Sample<T>::PushResult
    sample(Sample<T> &sample, Transform transform = {}) noexcept {
        const auto r = sample.push(transform(C::now() - t0));
        t0 = SENTINEL;
        return r;
    }

  private:
    // 0 is a sentinel for unitialized here
    static constexpr auto SENTINEL = std::chrono::time_point<C, D>{};
    std::chrono::time_point<C, D> t0 = SENTINEL;
};

} // namespace shclog::bench::time
