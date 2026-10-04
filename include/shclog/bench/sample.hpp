#pragma once

#include "shclog/cast.hpp"
#include "shclog/math.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <concepts>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <type_traits>
#include <utility>

#include "shclog/collections/slice.hpp"

namespace shclog::bench::sample {

using namespace shclog::collections::slice;

template <typename T>
    requires an_integer<T> || std::floating_point<T>
struct Sample {
  public:
    using TwithOverflow = shclog::math::Overflow<T>;

    std::unique_ptr<Slice<T>> samples;
    bool samples_sorted{false};
    usize count{0};

    enum class MakeError : u8 {
        OutOfMemory,
    };

    [[nodiscard]] static std::expected<Sample<T>, MakeError>
    make(const usize len) noexcept {
        auto slice = Slice<T>::make(len);
        if (!slice.has_value())
            return std::unexpected(MakeError::OutOfMemory);
        return Sample<T>(std::move(slice.value()));
    }

    enum class PushResult : u8 {
        Success,
        BufferFull,
    };

    [[nodiscard]] PushResult push(const T v) noexcept {
        if (count >= samples->len) [[unlikely]]
            return PushResult::BufferFull;
        samples_sorted = false;
        (*samples)[count++] = v;
        _min = std::min(_min, v);
        _max = std::max(_max, v);

        if (!_total.overflow) [[likely]] {
            _total = add_with_overflow(_total.value, v);
            if (_total.overflow) [[unlikely]]
                _total.value = std::numeric_limits<T>::max();
        }
        return PushResult::Success;
    }

    [[nodiscard]] std::span<const T> span() const noexcept {
        return std::span<const T>(samples->data, count);
    }

    [[nodiscard]] std::span<T> span() noexcept {
        return std::span<T>(samples->data, count);
    }

    enum class PercentileError : u8 {
        InvalidPercentile,
        EmptySamples,
    };

    [[nodiscard]] std::expected<T, PercentileError>
    percentile(const f64 p) noexcept {
        if (std::isnan(p) || p > 1.0 || p < 0.0) [[unlikely]]
            return std::unexpected(PercentileError::InvalidPercentile);

        if (count == 0) [[unlikely]]
            return std::unexpected(PercentileError::EmptySamples);

        if (!samples_sorted) {
            assert(count <= samples->len);
            std::ranges::sort(span());
            samples_sorted = true;
        }

        const usize idx = int_cast<usize>(p * float_cast<f64>(count - 1));
        return (*samples)[idx];
    }

    enum class AvgError : u8 {
        EmptySamples,
    };

    template <typename C = std::conditional_t<std::floating_point<T>, T, f64>>
        requires Arithmetic<C>
    [[nodiscard]] std::expected<C, AvgError> avg() const noexcept {
        static_assert(
            std::is_floating_point_v<C> ||
                (an_integer<T> && sizeof(C) > sizeof(T) &&
                 (std::is_signed_v<C> || std::is_unsigned_v<T>)),
            "avg<C> needs a floating C, or an integer C wider than T "
            "that keeps its sign, to recover from total overflow"
        );

        if (count == 0) [[unlikely]]
            return std::unexpected(AvgError::EmptySamples);

        if constexpr (std::is_floating_point_v<C>) {
            const C n = float_cast<C>(count);
            // floats don't floor, so dividing first costs nothing and keeps
            // every partial sum within the range of the samples
            if (_total.overflow) [[unlikely]]
                return std::transform_reduce(
                    span().begin(),
                    span().end(),
                    C{},
                    std::plus{},
                    [n](const T x) { return float_cast<C>(x) / n; }
                );
            return float_cast<C>(_total.value) / n;
        } else {
            const C n = int_cast<C>(count);
            if (_total.overflow) [[unlikely]]
                return std::reduce(span().begin(), span().end(), C{}) / n;
            return int_cast<C>(_total.value) / n;
        }
    }

    enum class SampleError : u8 {
        EmptySamples,
    };

    [[nodiscard]] std::expected<T, SampleError> min() const noexcept {
        if (count == 0) [[unlikely]]
            return std::unexpected(SampleError::EmptySamples);
        return _min;
    }

    [[nodiscard]] std::expected<T, SampleError> max() const noexcept {
        if (count == 0) [[unlikely]]
            return std::unexpected(SampleError::EmptySamples);
        return _max;
    }

    [[nodiscard]] std::expected<TwithOverflow, SampleError>
    total() const noexcept {
        if (count == 0) [[unlikely]]
            return std::unexpected(SampleError::EmptySamples);
        return _total;
    }

    [[nodiscard]] bool empty() const noexcept { return count == 0; }

  private:
    T _min = std::numeric_limits<T>::max();
    T _max = std::numeric_limits<T>::lowest();
    TwithOverflow _total{0, false};
    explicit Sample(std::unique_ptr<Slice<T>> slice)
        : samples(std::move(slice)) {}
};

} // namespace shclog::bench::sample
