#pragma once

#include "shclog/cast.hpp"
#include "shclog/const.hpp"
#include "shclog/math.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <new>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace shclog::collections::slice {

using namespace shclog::math;

// alignment is computed using max alignment of all fields
// since we only have size and T, T is more likely to be it
// at the very least it's equal to usize
template <typename T>
inline constexpr usize slice_default_align = std::max(alignof(T), alignof(T *));

template <typename A, typename B>
concept comparable_elements = same_cv<A, B> || (byte_like<A> && byte_like<B>);

struct NoSentinel {};
inline constexpr NoSentinel NO_SENTINEL{};

// This guy is literally just a fat ptr
// you are responsible for the ownership
// Sentinel is not counted if available
template <
    typename T,
    auto Sentinel = NO_SENTINEL,
    usize Align = slice_default_align<T>>
struct alignas(Align) Slice {
    static_assert(std::has_single_bit(Align));
    static_assert(Align >= slice_default_align<T>);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_trivially_destructible_v<T>);

  public:
    static constexpr bool HAS_SENTINEL =
        !same_cv<decltype(Sentinel), NoSentinel>;
    static_assert(
        !HAS_SENTINEL || same_cv<decltype(Sentinel), T>,
        "a sentinel has to be of type T, ex: Slice<u8, u8{0}>"
    );

    T *data;
    // notice you can change this, but it's up to you to restore it
    usize len;

    // notice there's no sense of ownership here
    Slice(T *const d, const usize n) noexcept : data(d), len(n) {}
    template <usize N> Slice(T (&arr)[N]) noexcept : data(arr), len(N) {
        if constexpr (HAS_SENTINEL) {
            assert(arr[N - 1] == Sentinel);
            --len;
        }
    }

    enum class MakeError : u8 {
        OutOfMemory,
    };

    // glue for ranges
    [[nodiscard]] T *begin() noexcept { return data; }
    [[nodiscard]] const T *begin() const noexcept { return data; }
    [[nodiscard]] T *end() noexcept { return data + len; }
    [[nodiscard]] const T *end() const noexcept { return data + len; }

    [[nodiscard]] static std::expected<std::unique_ptr<Slice>, MakeError>
    make(const usize len) noexcept {
        auto p = std::unique_ptr<Slice>(
            new (len + (HAS_SENTINEL ? 1 : 0), std::nothrow) Slice(len)
        );
        if (!p)
            return std::unexpected(MakeError::OutOfMemory);
        if constexpr (HAS_SENTINEL)
            p->data[len] = Sentinel;
        return p;
    }

    [[nodiscard]] decltype(auto)
    operator[](this auto &&self, usize i) noexcept {
        assert(i < self.len);
        return std::forward_like<decltype(self)>(self.data[i]);
    }

    // Sentinel can't be guaranteed on slice of a Slice
    [[nodiscard]] Slice<T, NO_SENTINEL, Align>
    first(const usize n) const noexcept {
        assert(n <= len);
        return Slice<T, NO_SENTINEL, Align>(data, n);
    }

    [[nodiscard]] std::string_view as_string_view() const noexcept
        requires byte_like<T> && (!std::is_volatile_v<T>)
    {
        return {ptr_cast<const char>(data), len};
    }

    template <std::ranges::contiguous_range R>
        requires(
            !std::is_array_v<std::remove_cvref_t<R>> &&
            comparable_elements<T, std::ranges::range_value_t<R>>
        )
    [[nodiscard]] friend bool operator==(const Slice &s, const R &r) noexcept {
        using E = std::ranges::range_value_t<R>;
        if constexpr (same_cv<T, E>)
            return std::ranges::equal(s, r);
        else
            return std::ranges::equal(
                s,
                r,
                {},
                [](const T a) { return std::bit_cast<u8>(a); },
                [](const E b) { return std::bit_cast<u8>(b); }
            );
    }

    template <typename E, usize N>
        requires comparable_elements<T, E>
    [[nodiscard]] friend bool operator==(const Slice &s, E (&r)[N]) noexcept {
        return s == collections::slice::Slice{r};
    }

    static void operator delete(void *const p) noexcept {
        ::operator delete(p, std::align_val_t{Align});
    }

    static void
    operator delete(void *const p, usize, const std::nothrow_t &) noexcept {
        ::operator delete(p, std::align_val_t{Align}, std::nothrow);
    }

  private:
    explicit Slice(const usize size) noexcept
        : data(ptr_cast<T>(this + 1))
        , len(size) {}

    [[nodiscard]] static void *operator new(
        const usize header,
        const usize payload,
        const std::nothrow_t &
    ) noexcept {
        // release this is UB, matching zig math
        if constexpr (IS_DEBUG) {
            [[maybe_unused]] auto [slice_byte_size, ovflw_slice_count] =
                mul_with_overflow(payload, sizeof(T));
            assert(!ovflw_slice_count);
            [[maybe_unused]] auto [total_byte_size, ovflw_total_count] =
                add_with_overflow(slice_byte_size, header);
            assert(!ovflw_total_count);
            return ::operator new(
                total_byte_size,
                std::align_val_t{Align},
                std::nothrow
            );
        }
        return ::operator new(
            header + payload * sizeof(T),
            std::align_val_t{Align},
            std::nothrow
        );
    }
};

// const char arrays are string literals, \0 terminated, the rest are plain
// arrays
template <typename C, usize N>
    requires char_like<C> && std::is_const_v<C>
Slice(C (&)[N]) -> Slice<C, std::remove_cv_t<C>{}>;

template <typename E, usize N>
    requires(!char_like<E> || !std::is_const_v<E>)
Slice(E (&)[N]) -> Slice<E>;

} // namespace shclog::collections::slice
