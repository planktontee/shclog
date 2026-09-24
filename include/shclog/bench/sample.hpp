#pragma once

#include "shclog/cast.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "shclog/collections/slice.hpp"

namespace shclog::bench::sample {

using namespace shclog::collections::slice;

template <typename T>
    requires an_integer<T> || std::floating_point<T>
struct Sample {
  public:
    T min = std::numeric_limits<T>::max();
    T max = std::numeric_limits<T>::lowest();
    // TODO: handle overflow
    T total{};
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
        if (count >= samples->len)
            return PushResult::BufferFull;
        samples_sorted = false;
        (*samples)[count++] = v;
        min = std::min(min, v);
        max = std::max(max, v);
        total += v;
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

    template <f64 p>
        requires(p >= 0.0 && p <= 1.0)
    T percentile() noexcept {
        const auto r = percentile(p);
        assert(r.has_value());
        return *r;
    }

    std::expected<T, PercentileError> percentile(const f64 p) noexcept {
        if (p > 1.0 || p < 0.0) [[unlikely]]
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

  private:
    explicit Sample(std::unique_ptr<Slice<T>> slice)
        : samples(std::move(slice)) {}
};

} // namespace shclog::bench::sample
