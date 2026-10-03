#include "shclog/doctest.hpp"

#include "shclog/bench/report.hpp"
#include "shclog/collections/slice.hpp"
#include "shclog/lang.hpp"
#include "shclog/types.hpp"
#include <limits>
#include <source_location>
#include <string_view>
#include <utility>

using namespace shclog;
using namespace shclog::lang;
using namespace shclog::bench::report;
using namespace shclog::collections::slice;

using Unit = TimeUnit::Unit;

static_assert(UnitFmt<TimeUnit, u64>);
static_assert(UnitFmt<TimeUnit, i32>);
static_assert(UnitFmt<TimeUnit, f64>);

template <Unit U, typename T>
void assert_repr(
    const T value, const std::u8string_view expected,
    const usize cap = TimeUnit::REPR_MAX_LEN<T>,
    const std::source_location loc = std::source_location::current()) {
    auto buf = unwrap(Slice<u8>::make(cap));
    const auto written = TimeUnit::write(value, U, *buf);
    // this will keep failures at the caller
    if (!written.has_value()) {
        ADD_FAIL_CHECK_AT(loc.file_name(), int_cast<int>(loc.line()),
                          "expected " << expected << ", got error "
                                      << std::to_underlying(written.error()));
        return;
    }
    const auto got = buf->first(*written);
    if (got != expected)
        ADD_FAIL_CHECK_AT(loc.file_name(), int_cast<int>(loc.line()),
                          "expected " << expected << ", got " << got);
}

template <Unit U, typename T>
void assert_repr(
    const T value, const UnitReprError expected,
    const usize cap = TimeUnit::REPR_MAX_LEN<T>,
    const std::source_location loc = std::source_location::current()) {
    auto buf = unwrap(Slice<u8>::make(cap));
    const auto written = TimeUnit::write(value, U, *buf);

    if (written.has_value()) {
        ADD_FAIL_CHECK_AT(loc.file_name(), int_cast<int>(loc.line()),
                          "expected error " << std::to_underlying(expected)
                                            << " with a " << cap
                                            << " byte buffer, got "
                                            << buf->first(*written));
        return;
    }
    if (written.error() != expected)
        ADD_FAIL_CHECK_AT(loc.file_name(), int_cast<int>(loc.line()),
                          "expected error "
                              << std::to_underlying(expected) << " with a "
                              << cap << " byte buffer, got error "
                              << std::to_underlying(written.error()));
}

TEST_CASE("TimeUnit::write moves up to the biggest unit the value reaches") {
    assert_repr<Unit::nanoseconds>(u64{1000}, u8"1 µs");
    assert_repr<Unit::microseconds>(u64{1000}, u8"1 ms");
    assert_repr<Unit::milliseconds>(u64{1000}, u8"1 s");
    assert_repr<Unit::seconds>(u64{60}, u8"1 m");
    assert_repr<Unit::minutes>(u64{60}, u8"1 h");
    assert_repr<Unit::nanoseconds>(u64{3'600'000'000'000}, u8"1 h");
}

TEST_CASE("TimeUnit::write stays put just below the next unit") {
    assert_repr<Unit::nanoseconds>(u64{999}, u8"999 ns");
    assert_repr<Unit::milliseconds>(u64{999}, u8"999 ms");
    assert_repr<Unit::seconds>(u64{59}, u8"59 s");
    assert_repr<Unit::minutes>(u64{59}, u8"59 m");
}

TEST_CASE("TimeUnit::write never goes below the unit it was given") {
    assert_repr<Unit::nanoseconds>(u64{0}, u8"0 ns");
    assert_repr<Unit::milliseconds>(u64{0}, u8"0 ms");
    assert_repr<Unit::hours>(u64{0}, u8"0 h");
    assert_repr<Unit::milliseconds>(0.5, u8"0.5 ms");
}

TEST_CASE("TimeUnit::write truncates to three characters") {
    assert_repr<Unit::nanoseconds>(u64{1100}, u8"1.1 µs");
    assert_repr<Unit::nanoseconds>(u64{1110}, u8"1.1 µs");
    assert_repr<Unit::nanoseconds>(u64{9100}, u8"9.1 µs");
    assert_repr<Unit::nanoseconds>(u64{10100}, u8"10 µs");
    assert_repr<Unit::nanoseconds>(u64{11000}, u8"11 µs");
    assert_repr<Unit::nanoseconds>(u64{999000}, u8"999 µs");
    assert_repr<Unit::nanoseconds>(u64{1999}, u8"1.9 µs");
    assert_repr<Unit::nanoseconds>(u64{19999}, u8"19 µs");
    assert_repr<Unit::nanoseconds>(u64{999'999}, u8"999 µs");
}

TEST_CASE("TimeUnit::write drops a zero fraction") {
    assert_repr<Unit::nanoseconds>(u64{1099}, u8"1 µs");
    assert_repr<Unit::nanoseconds>(u64{9}, u8"9 ns");
}

TEST_CASE("TimeUnit::write fractions follow the unit ratio, not 1000") {
    assert_repr<Unit::seconds>(u64{90}, u8"1.5 m");
    assert_repr<Unit::seconds>(u64{5400}, u8"1.5 h");
    assert_repr<Unit::seconds>(u64{119}, u8"1.9 m");
}

TEST_CASE("TimeUnit::write lets hours grow past three digits") {
    assert_repr<Unit::hours>(u64{5000}, u8"5000 h");
    assert_repr<Unit::seconds>(u64{18'000'000}, u8"5000 h");

    constexpr u64 max = std::numeric_limits<u64>::max();
    assert_repr<Unit::nanoseconds>(max, u8"5124095 h");
    assert_repr<Unit::hours>(max, u8"18446744073709551615 h");
}

TEST_CASE("TimeUnit::write takes any integer type") {
    assert_repr<Unit::nanoseconds>(u32{1200}, u8"1.2 µs");
    assert_repr<Unit::nanoseconds>(i32{1200}, u8"1.2 µs");
    assert_repr<Unit::nanoseconds>(i64{0}, u8"0 ns");
    assert_repr<Unit::seconds>(u8{255}, u8"4.2 m");
    assert_repr<Unit::nanoseconds>(u8{255}, u8"255 ns");
    assert_repr<Unit::nanoseconds>(u16{60'000}, u8"60 µs");
    assert_repr<Unit::nanoseconds>(i16{-32'768}, u8"-32 µs");
}

TEST_CASE("TimeUnit::write computes the integer tenth without overflowing") {
    // value * 10 would overflow i32 for both
    assert_repr<Unit::nanoseconds>(i32{2'000'000'000}, u8"2 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<i32>::max(), u8"2.1 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<i32>::min(), u8"-2.1 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<u32>::max(), u8"4.2 s");
}

TEST_CASE("TimeUnit::write ignores everything below the tenth") {
    // 1h 6min 39.999...s
    assert_repr<Unit::nanoseconds>(u64{3'999'999'999'999}, u8"1.1 h");
    assert_repr<Unit::nanoseconds>(i64{-3'999'999'999'999}, u8"-1.1 h");
    assert_repr<Unit::nanoseconds>(3'999'999'999'999.0, u8"1.1 h");
}

TEST_CASE("TimeUnit::write takes floating values") {
    assert_repr<Unit::nanoseconds>(1200.0, u8"1.2 µs");
    assert_repr<Unit::microseconds>(1.2, u8"1.2 µs");
    assert_repr<Unit::nanoseconds>(1.2e6, u8"1.2 ms");
    assert_repr<Unit::nanoseconds>(0.5, u8"0.5 ns");
    assert_repr<Unit::nanoseconds>(0.05, u8"0 ns");
    assert_repr<Unit::nanoseconds>(1234.5F, u8"1.2 µs");
    assert_repr<Unit::seconds>(90.0L, u8"1.5 m");
    assert_repr<Unit::nanoseconds>(4200.0, u8"4.2 µs");
    assert_repr<Unit::nanoseconds>(8.2e9, u8"8.2 s");
    assert_repr<Unit::microseconds>(-1.2, u8"-1.2 µs");
    assert_repr<Unit::nanoseconds>(-999'999.0, u8"-999 µs");
}

TEST_CASE("TimeUnit::write signs negative values") {
    assert_repr<Unit::nanoseconds>(i64{-1}, u8"-1 ns");
    assert_repr<Unit::nanoseconds>(i64{-1100}, u8"-1.1 µs");
    assert_repr<Unit::seconds>(i32{-90}, u8"-1.5 m");
    assert_repr<Unit::seconds>(i8{-128}, u8"-2.1 m");
}

TEST_CASE("TimeUnit::write handles the signed minimum") {
    constexpr i64 min = std::numeric_limits<i64>::min();
    assert_repr<Unit::nanoseconds>(min, u8"-2562047 h");
    assert_repr<Unit::hours>(min, u8"-9223372036854775808 h");
}

TEST_CASE("TimeUnit::write doesn't print -0") {
    assert_repr<Unit::nanoseconds>(-0.0, u8"0 ns");
    assert_repr<Unit::nanoseconds>(-0.05, u8"0 ns");
    assert_repr<Unit::nanoseconds>(-0.5, u8"-0.5 ns");
}

TEST_CASE("TimeUnit::write rejects values it can't represent") {
    assert_repr<Unit::nanoseconds>(std::numeric_limits<f64>::quiet_NaN(),
                                   UnitReprError::OutOfRange);
    assert_repr<Unit::nanoseconds>(std::numeric_limits<f64>::infinity(),
                                   UnitReprError::OutOfRange);
    assert_repr<Unit::nanoseconds>(-std::numeric_limits<f64>::infinity(),
                                   UnitReprError::OutOfRange);
}

TEST_CASE("TimeUnit::write returns how many bytes it wrote") {
    auto buf = unwrap(Slice<u8>::make(TimeUnit::REPR_MAX_LEN<u64>));
    const usize n = unwrap(TimeUnit::write(u64{1100}, Unit::nanoseconds, *buf));
    // the micro sign takes 2 bytes
    CHECK(n == 7);
    CHECK(buf->first(n) == u8"1.1 µs");
}

TEST_CASE("TimeUnit::write fits an exactly sized buffer") {
    assert_repr<Unit::seconds>(u64{1}, u8"1 s", 3);
    assert_repr<Unit::seconds>(u64{1}, UnitReprError::BufferTooSmall, 2);
}

TEST_CASE("TimeUnit::write reports a short buffer at every piece") {
    // "-1.1 µs" is 8 bytes, every shorter buffer cuts into a different piece
    for (usize cap = 0; cap < 8; ++cap)
        assert_repr<Unit::nanoseconds>(i64{-1100},
                                       UnitReprError::BufferTooSmall, cap);
    assert_repr<Unit::nanoseconds>(i64{-1100}, u8"-1.1 µs", 8);
    assert_repr<Unit::microseconds>(-1.1F, UnitReprError::BufferTooSmall, 7);
}

template <typename T> void check_widest(const Unit u) {
    using L = std::numeric_limits<T>;
    auto buf = unwrap(Slice<u8>::make(TimeUnit::REPR_MAX_LEN<T>));
    CHECK(TimeUnit::write(L::max(), u, *buf).has_value());
    CHECK(TimeUnit::write(L::lowest(), u, *buf).has_value());
}

TEST_CASE("TimeUnit::REPR_MAX_LEN fits the widest value of each type") {
    for (const Unit u :
         {Unit::nanoseconds, Unit::microseconds, Unit::milliseconds,
          Unit::seconds, Unit::minutes, Unit::hours}) {
        CAPTURE(std::to_underlying(u));
        check_widest<u8>(u);
        check_widest<i8>(u);
        check_widest<u64>(u);
        check_widest<i64>(u);
        check_widest<f32>(u);
        check_widest<f64>(u);
        check_widest<f80>(u);
    }
}
