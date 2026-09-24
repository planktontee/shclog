#pragma once

#include "shclog/cast.hpp"
#include "shclog/types.hpp"
#include <bit>
#include <cassert>
#include <expected>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace shclog::collections::slice {

// alignment is computed using max alignment of all fields
// since we only have size and T, T is more likely to be it
// at the very least it's equal to usize
template <typename T>
inline constexpr usize slice_default_align = std::max(alignof(T), alignof(T *));

// This guy is literally just a fat ptr
// you are responsible for the ownership
template <typename T, usize Align = slice_default_align<T>>
struct alignas(Align) Slice {
    static_assert(std::has_single_bit(Align));
    static_assert(Align >= slice_default_align<T>);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_trivially_destructible_v<T>);

  public:
    T *data;
    // notice you can change this, but it's up to you to restore it
    usize len;

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
        auto p = std::unique_ptr<Slice>(new (len, std::nothrow) Slice(len));
        if (!p)
            return std::unexpected(MakeError::OutOfMemory);
        return p;
    }

    [[nodiscard]] decltype(auto) operator[](this auto &&self,
                                            usize i) noexcept {
        assert(i < self.len);
        return std::forward_like<decltype(self)>(self.data[i]);
    }

    static void operator delete(void *const p) noexcept {
        ::operator delete(p, std::align_val_t{Align});
    }

    static void operator delete(void *const p, usize,
                                const std::nothrow_t &) noexcept {
        ::operator delete(p, std::align_val_t{Align}, std::nothrow);
    }

  private:
    explicit Slice(const usize size) noexcept
        : data(ptr_cast<T>(this + 1)), len(size) {}

    static void *operator new(const usize header, const usize payload,
                              const std::nothrow_t &) noexcept {
        // this can overflow, need to handle that and return null
        return ::operator new(header + payload * sizeof(T),
                              std::align_val_t{Align}, std::nothrow);
    }
};

} // namespace shclog::collections::slice
