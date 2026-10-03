#pragma once

#include "shclog/cast.hpp"
#include "shclog/collections/slice.hpp"
#include "shclog/types.hpp"
#include <cassert>
#include <charconv>
#include <concepts>
#include <expected>
#include <system_error>

namespace shclog {

enum class WriteError : u8 {
    BufferTooSmall,
};

template <an_integer T>
[[nodiscard]] std::expected<usize, WriteError>
write_number(const collections::slice::Slice<u8> &buf, const T v,
             const int base = 10) noexcept {
    char *const first = ptr_cast<char>(buf.data);
    const auto [end, ec] = std::to_chars(first, first + buf.len, v, base);

    if (ec == std::errc::value_too_large) [[unlikely]]
        return std::unexpected(WriteError::BufferTooSmall);
    assert(ec == std::errc{});
    return int_cast<usize>(end - first);
}

template <std::floating_point T>
[[nodiscard]] std::expected<usize, WriteError>
write_number(const collections::slice::Slice<u8> &buf, const T v,
             const std::chars_format fmt = std::chars_format::fixed,
             const int precision = 0) noexcept {
    char *const first = ptr_cast<char>(buf.data);
    const auto [end, ec] =
        std::to_chars(first, first + buf.len, v, fmt, precision);

    if (ec == std::errc::value_too_large) [[unlikely]]
        return std::unexpected(WriteError::BufferTooSmall);
    assert(ec == std::errc{});
    return int_cast<usize>(end - first);
}

} // namespace shclog
