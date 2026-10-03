#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace shclog {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;
using f80 = long double;

using usize = size_t;
using isize = intptr_t;

using c_int = int;
using c_long = long;

template <typename A, typename B>
concept same_cv = std::same_as<std::remove_cv_t<A>, std::remove_cv_t<B>>;

template <typename T>
concept byte_like = same_cv<T, u8> || same_cv<T, char> ||
                    same_cv<T, char8_t> || same_cv<T, signed char> ||
                    same_cv<T, std::byte>;

template <typename T>
concept char_like = same_cv<T, char> || same_cv<T, char8_t>;
} // namespace shclog
