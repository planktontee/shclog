#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace shclog::align {
template <typename T, size_t A> struct alignas(A) PadAlignBy {
    static_assert(A >= sizeof(T));
    static_assert(A % alignof(T) == 0);

  private:
    char padding[A - sizeof(T)];

  public:
    T value;

    PadAlignBy() noexcept : value() {};

    template <typename _T>
        requires(std::is_trivially_copyable_v<_T> &&
                 std::is_convertible_v<T, _T>)
    explicit PadAlignBy(_T v) noexcept : value(v) {}

    template <typename _T>
        requires(!std::is_trivially_copyable_v<_T> &&
                 std::is_same_v<std::decay<T>, std::decay<_T>>)
    explicit PadAlignBy(_T &&v) noexcept : value(std::move(v)) {}

    PadAlignBy(PadAlignBy<T, A> &) = delete;
    PadAlignBy(PadAlignBy<T, A> &&) = delete;
};

template <typename T>
using CachePadAlign =
    align::PadAlignBy<T, std::hardware_destructive_interference_size>;

} // namespace shclog::align
