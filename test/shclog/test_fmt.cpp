#include "shclog/doctest.hpp"

#include "shclog/collections/slice.hpp"
#include "shclog/fmt.hpp"
#include "shclog/lang.hpp"
#include "shclog/types.hpp"
#include <charconv>
#include <limits>

using namespace shclog;
using namespace shclog::lang;
using namespace shclog::collections::slice;

TEST_CASE("write_number writes integers in base 10") {
    auto buf = unwrap(Slice<u8>::make(64));
    usize n = unwrap(write_number(*buf, u64{0}));
    CHECK(buf->first(n) == "0");
    n = unwrap(write_number(*buf, i32{-42}));
    CHECK(buf->first(n) == "-42");
    n = unwrap(write_number(*buf, std::numeric_limits<u64>::max()));
    CHECK(buf->first(n) == "18446744073709551615");
    n = unwrap(write_number(*buf, std::numeric_limits<i64>::min()));
    CHECK(buf->first(n) == "-9223372036854775808");
    n = unwrap(write_number(*buf, u8{255}));
    CHECK(buf->first(n) == "255");
}

TEST_CASE("write_number takes an integer base") {
    auto buf = unwrap(Slice<u8>::make(64));
    usize n = unwrap(write_number(*buf, u8{255}, 16));
    CHECK(buf->first(n) == "ff");
    n = unwrap(write_number(*buf, i32{-5}, 2));
    CHECK(buf->first(n) == "-101");
    n = unwrap(write_number(*buf, u64{35}, 36));
    CHECK(buf->first(n) == "z");
}

TEST_CASE("write_number defaults floats to fixed with no decimals") {
    auto buf = unwrap(Slice<u8>::make(64));
    usize n = unwrap(write_number(*buf, 1.4));
    CHECK(buf->first(n) == "1");
    n = unwrap(write_number(*buf, 0.1));
    CHECK(buf->first(n) == "0");
    n = unwrap(write_number(*buf, 1e30));
    CHECK(buf->first(n) == "1000000000000000019884624838656");
    n = unwrap(write_number(*buf, -0.0));
    CHECK(buf->first(n) == "-0");
    n = unwrap(write_number(*buf, 2.0F));
    CHECK(buf->first(n) == "2");
}

TEST_CASE("write_number takes a float format and precision") {
    auto buf = unwrap(Slice<u8>::make(64));
    usize n =
        unwrap(write_number(*buf, 1e30, std::chars_format::scientific, 2));
    CHECK(buf->first(n) == "1.00e+30");
    n = unwrap(write_number(*buf, 2.0 / 3.0, std::chars_format::fixed, 2));
    CHECK(buf->first(n) == "0.67");
}

TEST_CASE("write_number returns how many bytes it wrote") {
    auto buf = unwrap(Slice<u8>::make(8));
    const usize n = unwrap(write_number(*buf, u32{123}));
    CHECK(n == 3);
    CHECK(buf->first(n) == "123");
}

TEST_CASE("write_number reports a buffer that can't fit the number") {
    auto exact = unwrap(Slice<u8>::make(3));
    usize n = unwrap(write_number(*exact, u32{123}));
    CHECK(exact->first(n) == "123");

    auto small = unwrap(Slice<u8>::make(2));
    auto written = write_number(*small, u32{123});
    REQUIRE(!written.has_value());
    CHECK(written.error() == WriteError::BufferTooSmall);

    auto empty = unwrap(Slice<u8>::make(0));
    written = write_number(*empty, u32{0});
    REQUIRE(!written.has_value());
    CHECK(written.error() == WriteError::BufferTooSmall);
}
