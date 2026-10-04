#include "shclog/doctest.hpp"

#include "shclog/collections/slice.hpp"
#include "shclog/lang.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace shclog;
using namespace shclog::lang;
using namespace shclog::collections::slice;

static_assert(byte_like<const u8> && byte_like<const volatile std::byte>);
static_assert(char_like<const char8_t> && !char_like<const u8>);

static_assert(!Slice<u8>::HAS_SENTINEL);
static_assert(Slice<u8, u8{0}>::HAS_SENTINEL);
// a slice of a sentinel slice ends wherever it ends, not at the sentinel
static_assert(std::same_as<
              decltype(std::declval<const Slice<u8, u8{0}> &>().first(1)),
              Slice<u8>>);

TEST_CASE("Slice views a char array up to its \\0 sentinel") {
    const Slice s{u8"µs"};
    static_assert(
        std::same_as<decltype(s), const Slice<const char8_t, u8'\0'>>
    );
    // µ takes 2 bytes
    CHECK(s.len == 3);
    CHECK(s.data[s.len] == u8'\0');
}

TEST_CASE("Slice views a mutable char array as a plain buffer") {
    char buf[]{'a', 'b', 'c', 'd'};
    const Slice s{buf};
    static_assert(std::same_as<decltype(s), const Slice<char>>);
    CHECK(s.data == buf);
    CHECK(s.len == 4);

    char8_t text[] = u8"ab";
    static_assert(std::same_as<decltype(Slice{text}), Slice<char8_t>>);
    CHECK(Slice{text}.len == 3);
}

TEST_CASE("Slice == keeps a mutable char array's last byte") {
    char buf[]{'a', 'b', 'c', 'd'};
    char text[] = "ab";
    CHECK(Slice{"abcd"} == buf);
    const bool reversed = buf == Slice{"abcd"};
    CHECK(reversed);
    CHECK(Slice{"ab"} != text);
    CHECK(Slice{"ab"} == Slice{text}.first(2));
}

TEST_CASE("Slice views every element of any other array") {
    const u32 elems[]{1, 2, 3};
    const Slice s{elems};
    static_assert(std::same_as<decltype(s), const Slice<const u32>>);
    CHECK(s.data == elems);
    CHECK(s.len == 3);
}

TEST_CASE("Slice::make with a sentinel stores it at data[len]") {
    auto s = unwrap(Slice<u8, u8{0}>::make(3));
    std::ranges::copy(Slice{u8"µs"}, s->begin());
    CHECK(s->len == 3);
    CHECK(s->data[3] == 0);
    // the sentinel isn't part of the elements
    CHECK(*s == "µs");
}

TEST_CASE("Slice::first shares data and shortens len") {
    const Slice s{u8"µs"};
    const auto f = s.first(2);
    CHECK(f.data == s.data);
    CHECK(f.len == 2);
    CHECK(s.first(0).len == 0);
    CHECK(s.first(3) == s);
}

TEST_CASE("Slice == takes char arrays as \\0 sentinel arrays") {
    const Slice s{u8"µs"};
    CHECK(s == u8"µs");
    CHECK(s == "µs");
    CHECK(u8"µs" == s);
    CHECK(s != "µ");
    CHECK(s != "µs ");
    CHECK(s.first(0) == "");
}

TEST_CASE("Slice == compares a \\0 inside a char array like any other byte") {
    const char text[]{'a', '\0', 'b', '\0'};
    const char other[]{'a', '\0', 'c', '\0'};
    const Slice s{text};
    CHECK(s.len == 3);
    CHECK(s == text);
    CHECK(s != other);
    CHECK(s != "a");
}

TEST_CASE("Slice == takes every element of a non-char array") {
    const Slice s{u8"µs"};
    const u8 bytes[3]{0xC2, 0xB5, 's'};
    const u8 with_zero[4]{0xC2, 0xB5, 's', 0};
    CHECK(s == bytes);
    CHECK(s != with_zero);
}

TEST_CASE("Slice == compares byte-like elements as bytes") {
    // µ's first byte, 0xC2, is negative as a char and still has to match
    const Slice s{u8"µs"};
    CHECK(s == std::string("µs"));
    CHECK(s == std::u8string(u8"µs"));
    CHECK(s == std::vector<u8>{0xC2, 0xB5, 's'});
    CHECK(
        s ==
        std::array<std::byte, 3>{
            std::byte{0xC2},
            std::byte{0xB5},
            std::byte{'s'}
        }
    );
    CHECK(s != std::vector<u8>{0xC2, 0xB5});
}

TEST_CASE("Slice == compares other slices") {
    const Slice a{u8"µs"};
    const Slice b{"µs"};
    CHECK(a == b);
    CHECK(a.first(2) != b);
}

TEST_CASE("Slice == compares non-byte elements by value") {
    const u32 elems[]{1, 2, 3};
    const Slice s{elems};
    CHECK(s == std::vector<u32>{1, 2, 3});
    CHECK(s != std::vector<u32>{1, 2, 4});
}

template <typename S>
concept has_string_view = requires(const S &s) { s.as_string_view(); };

static_assert(has_string_view<Slice<u8>>);
static_assert(has_string_view<Slice<const char8_t>>);
static_assert(!has_string_view<Slice<u32>>);

TEST_CASE("Slice::as_string_view views the same bytes") {
    auto s = unwrap(Slice<u8>::make(2));
    (*s)[0] = 'o';
    (*s)[1] = 'k';
    const std::string_view v = s->as_string_view();
    CHECK(v == "ok");
    CHECK(
        static_cast<const void *>(v.data()) ==
        static_cast<const void *>(s->data)
    );
}

TEST_CASE("Slice::as_string_view leaves the sentinel out and follows first") {
    const Slice s{u8"µs"};
    CHECK(s.as_string_view() == "µs");
    CHECK(s.as_string_view().size() == 3);
    CHECK(s.first(2).as_string_view() == "µ");

    std::byte bytes[] = {std::byte{'h'}, std::byte{'i'}};
    CHECK(Slice{bytes}.as_string_view() == "hi");
}
