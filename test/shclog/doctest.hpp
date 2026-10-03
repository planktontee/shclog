#pragma once

#include <doctest/doctest.h>

#include "shclog/collections/slice.hpp"
#include "shclog/types.hpp"
#include <bit>
#include <cstddef>
#include <string>
#include <string_view>

namespace shclog::test {
template <typename R> doctest::String visible(const R &bytes) {
    std::string out;
    for (const auto c : bytes) {
        switch (const char ch = std::bit_cast<char>(c)) {
        case '\0':
            out += "␀";
            break;
        case '\n':
            out += "␊";
            break;
        case '\r':
            out += "␍";
            break;
        case '\t':
            out += "␉";
            break;
        default:
            out.push_back(ch);
        }
    }
    out += "␃";
    return out;
}
} // namespace shclog::test

namespace doctest {

template <typename T, auto Sentinel, shclog::usize Align>
    requires shclog::byte_like<T>
struct StringMaker<shclog::collections::slice::Slice<T, Sentinel, Align>> {
    static String
    convert(const shclog::collections::slice::Slice<T, Sentinel, Align> &s) {
        return shclog::test::visible(s);
    }
};

template <> struct StringMaker<std::u8string_view> {
    static String convert(const std::u8string_view s) {
        return shclog::test::visible(s);
    }
};

template <std::size_t N> struct StringMaker<char8_t[N]> {
    static String convert(const char8_t (&s)[N]) {
        return shclog::test::visible(std::u8string_view(s, N - 1));
    }
};

} // namespace doctest
