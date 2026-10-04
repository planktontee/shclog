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
using Bytes = ByteUnit::Unit;

template <usize... Scales> struct FakeScale {
    enum class Unit : u8 {};
    static constexpr usize SCALES[] = {Scales...};
    static constexpr Unit LAST = static_cast<Unit>(sizeof...(Scales) - 1);
    static constexpr usize scale(const Unit u) noexcept {
        return SCALES[std::to_underlying(u)];
    }
    static constexpr std::string_view symbol(const Unit) noexcept {
        return "x";
    }
};

static_assert(UnitScale<TimeScale>);
static_assert(UnitScale<ByteScale>);
static_assert(UnitScale<FakeScale<1, 10, 100>>);
static_assert(!UnitScale<FakeScale<1, 3, 4>>);
static_assert(!UnitScale<FakeScale<1, std::numeric_limits<usize>::max()>>);
static_assert(UnitScale<FakeScale<1, std::numeric_limits<usize>::max() / 10>>);
static_assert(UnitFmt<TimeUnit, u64>);
static_assert(UnitFmt<TimeUnit, i32>);
static_assert(UnitFmt<TimeUnit, f64>);
static_assert(UnitFmt<ByteUnit, u64>);
static_assert(UnitFmt<ByteUnit, i32>);
static_assert(UnitFmt<ByteUnit, f64>);

template <typename E> struct repr_of;
template <> struct repr_of<Unit> {
    using type = TimeUnit;
};
template <> struct repr_of<Bytes> {
    using type = ByteUnit;
};

template <auto U> using Repr = repr_of<decltype(U)>::type;

template <auto U, typename T>
void assert_repr(
    const T value,
    const std::string_view expected,
    const usize cap = Repr<U>::template REPR_MAX_LEN<T>,
    const std::source_location loc = std::source_location::current()
) {
    auto buf = unwrap(Slice<u8>::make(cap));
    const auto written = Repr<U>::write(value, U, *buf);
    // this will keep failures at the caller
    if (!written.has_value()) {
        ADD_FAIL_CHECK_AT(
            loc.file_name(),
            int_cast<int>(loc.line()),
            "expected " << expected << ", got error "
                        << std::to_underlying(written.error())
        );
        return;
    }
    const auto got = buf->first(*written);
    if (got != expected)
        ADD_FAIL_CHECK_AT(
            loc.file_name(),
            int_cast<int>(loc.line()),
            "expected " << expected << ", got " << got
        );
}

template <auto U, typename T>
void assert_repr(
    const T value,
    const UnitReprError expected,
    const usize cap = Repr<U>::template REPR_MAX_LEN<T>,
    const std::source_location loc = std::source_location::current()
) {
    auto buf = unwrap(Slice<u8>::make(cap));
    const auto written = Repr<U>::write(value, U, *buf);

    if (written.has_value()) {
        ADD_FAIL_CHECK_AT(
            loc.file_name(),
            int_cast<int>(loc.line()),
            "expected error " << std::to_underlying(expected) << " with a "
                              << cap << " byte buffer, got "
                              << buf->first(*written)
        );
        return;
    }
    if (written.error() != expected)
        ADD_FAIL_CHECK_AT(
            loc.file_name(),
            int_cast<int>(loc.line()),
            "expected error " << std::to_underlying(expected) << " with a "
                              << cap << " byte buffer, got error "
                              << std::to_underlying(written.error())
        );
}

TEST_CASE("TimeUnit::write moves up to the biggest unit the value reaches") {
    assert_repr<Unit::nanoseconds>(u64{1000}, "1 µs");
    assert_repr<Unit::microseconds>(u64{1000}, "1 ms");
    assert_repr<Unit::milliseconds>(u64{1000}, "1 s");
    assert_repr<Unit::seconds>(u64{60}, "1 m");
    assert_repr<Unit::minutes>(u64{60}, "1 h");
    assert_repr<Unit::nanoseconds>(u64{3'600'000'000'000}, "1 h");
}

TEST_CASE("TimeUnit::write stays put just below the next unit") {
    assert_repr<Unit::nanoseconds>(u64{999}, "999 ns");
    assert_repr<Unit::milliseconds>(u64{999}, "999 ms");
    assert_repr<Unit::seconds>(u64{59}, "59 s");
    assert_repr<Unit::minutes>(u64{59}, "59 m");
}

TEST_CASE("TimeUnit::write never goes below the unit it was given") {
    assert_repr<Unit::nanoseconds>(u64{0}, "0 ns");
    assert_repr<Unit::milliseconds>(u64{0}, "0 ms");
    assert_repr<Unit::hours>(u64{0}, "0 h");
    assert_repr<Unit::milliseconds>(0.5, "0.5 ms");
}

TEST_CASE("TimeUnit::write truncates to three characters") {
    assert_repr<Unit::nanoseconds>(u64{1100}, "1.1 µs");
    assert_repr<Unit::nanoseconds>(u64{1110}, "1.1 µs");
    assert_repr<Unit::nanoseconds>(u64{9100}, "9.1 µs");
    assert_repr<Unit::nanoseconds>(u64{10100}, "10 µs");
    assert_repr<Unit::nanoseconds>(u64{11000}, "11 µs");
    assert_repr<Unit::nanoseconds>(u64{999000}, "999 µs");
    assert_repr<Unit::nanoseconds>(u64{1999}, "1.9 µs");
    assert_repr<Unit::nanoseconds>(u64{19999}, "19 µs");
    assert_repr<Unit::nanoseconds>(u64{999'999}, "999 µs");
}

TEST_CASE("TimeUnit::write drops a zero fraction") {
    assert_repr<Unit::nanoseconds>(u64{1099}, "1 µs");
    assert_repr<Unit::nanoseconds>(u64{9}, "9 ns");
}

TEST_CASE("TimeUnit::write fractions follow the unit ratio, not 1000") {
    assert_repr<Unit::seconds>(u64{90}, "1.5 m");
    assert_repr<Unit::seconds>(u64{5400}, "1.5 h");
    assert_repr<Unit::seconds>(u64{119}, "1.9 m");
}

TEST_CASE("TimeUnit::write lets hours grow past three digits") {
    assert_repr<Unit::hours>(u64{5000}, "5000 h");
    assert_repr<Unit::seconds>(u64{18'000'000}, "5000 h");

    constexpr u64 max = std::numeric_limits<u64>::max();
    assert_repr<Unit::nanoseconds>(max, "5124095 h");
    assert_repr<Unit::hours>(max, "18446744073709551615 h");
}

TEST_CASE("TimeUnit::write takes any integer type") {
    assert_repr<Unit::nanoseconds>(u32{1200}, "1.2 µs");
    assert_repr<Unit::nanoseconds>(i32{1200}, "1.2 µs");
    assert_repr<Unit::nanoseconds>(i64{0}, "0 ns");
    assert_repr<Unit::seconds>(u8{255}, "4.2 m");
    assert_repr<Unit::nanoseconds>(u8{255}, "255 ns");
    assert_repr<Unit::nanoseconds>(u16{60'000}, "60 µs");
    assert_repr<Unit::nanoseconds>(i16{-32'768}, "-32 µs");
}

TEST_CASE("TimeUnit::write computes the integer tenth without overflowing") {
    // value * 10 would overflow i32 for both
    assert_repr<Unit::nanoseconds>(i32{2'000'000'000}, "2 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<i32>::max(), "2.1 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<i32>::min(), "-2.1 s");
    assert_repr<Unit::nanoseconds>(std::numeric_limits<u32>::max(), "4.2 s");
}

TEST_CASE("TimeUnit::write ignores everything below the tenth") {
    // 1h 6min 39.999...s
    assert_repr<Unit::nanoseconds>(u64{3'999'999'999'999}, "1.1 h");
    assert_repr<Unit::nanoseconds>(i64{-3'999'999'999'999}, "-1.1 h");
    assert_repr<Unit::nanoseconds>(3'999'999'999'999.0, "1.1 h");
}

TEST_CASE("TimeUnit::write takes floating values") {
    assert_repr<Unit::nanoseconds>(1200.0, "1.2 µs");
    assert_repr<Unit::microseconds>(1.2, "1.2 µs");
    assert_repr<Unit::nanoseconds>(1.2e6, "1.2 ms");
    assert_repr<Unit::nanoseconds>(0.5, "0.5 ns");
    assert_repr<Unit::nanoseconds>(0.05, "0 ns");
    assert_repr<Unit::nanoseconds>(1234.5F, "1.2 µs");
    assert_repr<Unit::seconds>(90.0L, "1.5 m");
    assert_repr<Unit::nanoseconds>(4200.0, "4.2 µs");
    assert_repr<Unit::nanoseconds>(8.2e9, "8.2 s");
    assert_repr<Unit::microseconds>(-1.2, "-1.2 µs");
    assert_repr<Unit::nanoseconds>(-999'999.0, "-999 µs");
}

TEST_CASE("TimeUnit::write signs negative values") {
    assert_repr<Unit::nanoseconds>(i64{-1}, "-1 ns");
    assert_repr<Unit::nanoseconds>(i64{-1100}, "-1.1 µs");
    assert_repr<Unit::seconds>(i32{-90}, "-1.5 m");
    assert_repr<Unit::seconds>(i8{-128}, "-2.1 m");
}

TEST_CASE("TimeUnit::write handles the signed minimum") {
    constexpr i64 min = std::numeric_limits<i64>::min();
    assert_repr<Unit::nanoseconds>(min, "-2562047 h");
    assert_repr<Unit::hours>(min, "-9223372036854775808 h");
}

TEST_CASE("TimeUnit::write doesn't print -0") {
    assert_repr<Unit::nanoseconds>(-0.0, "0 ns");
    assert_repr<Unit::nanoseconds>(-0.05, "0 ns");
    assert_repr<Unit::nanoseconds>(-0.5, "-0.5 ns");
}

TEST_CASE("TimeUnit::write rejects values it can't represent") {
    assert_repr<Unit::nanoseconds>(
        std::numeric_limits<f64>::quiet_NaN(),
        UnitReprError::OutOfRange
    );
    assert_repr<Unit::nanoseconds>(
        std::numeric_limits<f64>::infinity(),
        UnitReprError::OutOfRange
    );
    assert_repr<Unit::nanoseconds>(
        -std::numeric_limits<f64>::infinity(),
        UnitReprError::OutOfRange
    );
}

TEST_CASE("TimeUnit::write returns how many bytes it wrote") {
    auto buf = unwrap(Slice<u8>::make(TimeUnit::REPR_MAX_LEN<u64>));
    const usize n = unwrap(TimeUnit::write(u64{1100}, Unit::nanoseconds, *buf));
    // the micro sign takes 2 bytes
    CHECK(n == 7);
    CHECK(buf->first(n) == "1.1 µs");
}

TEST_CASE("TimeUnit::write fits an exactly sized buffer") {
    assert_repr<Unit::seconds>(u64{1}, "1 s", 3);
    assert_repr<Unit::seconds>(u64{1}, UnitReprError::BufferTooSmall, 2);
}

TEST_CASE("TimeUnit::write reports a short buffer at every piece") {
    // "-1.1 µs" is 8 bytes, every shorter buffer cuts into a different piece
    for (usize cap = 0; cap < 8; ++cap)
        assert_repr<Unit::nanoseconds>(
            i64{-1100},
            UnitReprError::BufferTooSmall,
            cap
        );
    assert_repr<Unit::nanoseconds>(i64{-1100}, "-1.1 µs", 8);
    assert_repr<Unit::microseconds>(-1.1F, UnitReprError::BufferTooSmall, 7);
}

template <class R, typename T> void check_widest(const typename R::Unit u) {
    using L = std::numeric_limits<T>;
    auto buf = unwrap(Slice<u8>::make(R::template REPR_MAX_LEN<T>));
    CHECK(R::write(L::max(), u, *buf).has_value());
    CHECK(R::write(L::lowest(), u, *buf).has_value());
}

template <class R> void check_widest_every_unit() {
    for (usize i = 0; i <= R::LAST_UNIT; ++i) {
        const auto u = R::from(i);
        CAPTURE(i);
        check_widest<R, u8>(u);
        check_widest<R, i8>(u);
        check_widest<R, u64>(u);
        check_widest<R, i64>(u);
        check_widest<R, f32>(u);
        check_widest<R, f64>(u);
        check_widest<R, f80>(u);
    }
}

TEST_CASE("TimeUnit::REPR_MAX_LEN fits the widest value of each type") {
    check_widest_every_unit<TimeUnit>();
}

TEST_CASE("TimeUnit::ratio divides the scales of two units") {
    CHECK(TimeUnit::ratio(Unit::nanoseconds, Unit::nanoseconds) == 1);
    CHECK(TimeUnit::ratio(Unit::nanoseconds, Unit::seconds) == 1'000'000'000);
    CHECK(TimeUnit::ratio(Unit::seconds, Unit::hours) == 3600);
    CHECK(TimeUnit::ratio(Unit::nanoseconds, Unit::hours) == 3'600'000'000'000);
}

TEST_CASE("ByteUnit::write moves up to the biggest unit the value reaches") {
    assert_repr<Bytes::bytes>(u64{1024}, "1 KiB");
    assert_repr<Bytes::kibibytes>(u64{1024}, "1 MiB");
    assert_repr<Bytes::mebibytes>(u64{1024}, "1 GiB");
    assert_repr<Bytes::gibibytes>(u64{1024}, "1 TiB");
    assert_repr<Bytes::tebibytes>(u64{1024}, "1 PiB");
    assert_repr<Bytes::pebibytes>(u64{1024}, "1 EiB");
    assert_repr<Bytes::bytes>(u64{1} << 30, "1 GiB");
    assert_repr<Bytes::bytes>(u64{1} << 60, "1 EiB");
}

TEST_CASE("ByteUnit::write stays put just below the next unit") {
    assert_repr<Bytes::bytes>(u64{1023}, "1023 B");
    assert_repr<Bytes::kibibytes>(u64{1023}, "1023 KiB");
    assert_repr<Bytes::bytes>(u64{1000}, "1000 B");
    assert_repr<Bytes::bytes>((u64{1} << 20) - 1, "1023 KiB");
}

TEST_CASE("ByteUnit::write never goes below the unit it was given") {
    assert_repr<Bytes::bytes>(u64{0}, "0 B");
    assert_repr<Bytes::gibibytes>(u64{0}, "0 GiB");
    assert_repr<Bytes::exbibytes>(u64{1}, "1 EiB");
    assert_repr<Bytes::mebibytes>(0.5, "0.5 MiB");
}

TEST_CASE("ByteUnit::write fractions follow 1024, not 1000") {
    assert_repr<Bytes::bytes>(u64{1536}, "1.5 KiB");
    assert_repr<Bytes::bytes>(u64{1535}, "1.4 KiB");
    assert_repr<Bytes::bytes>(u64{1126}, "1 KiB");
    assert_repr<Bytes::bytes>(u64{1127}, "1.1 KiB");
    assert_repr<Bytes::bytes>(u64{2047}, "1.9 KiB");
    assert_repr<Bytes::bytes>(u64{10239}, "9.9 KiB");
    assert_repr<Bytes::bytes>(u64{10240}, "10 KiB");
    assert_repr<Bytes::kibibytes>(u64{1536}, "1.5 MiB");
}

TEST_CASE("ByteUnit::write lets exbibytes grow past four digits") {
    constexpr u64 max = std::numeric_limits<u64>::max();
    assert_repr<Bytes::bytes>(max, "15 EiB");
    assert_repr<Bytes::bytes>(std::numeric_limits<i64>::max(), "7.9 EiB");
    assert_repr<Bytes::bytes>(std::numeric_limits<i64>::min(), "-8 EiB");
    assert_repr<Bytes::kibibytes>(max, "16383 EiB");
    assert_repr<Bytes::mebibytes>(max, "16777215 EiB");
    assert_repr<Bytes::exbibytes>(max, "18446744073709551615 EiB");
    assert_repr<Bytes::bytes>(0x1p70, "1024 EiB");
    assert_repr<Bytes::bytes>(0x1.8p70, "1536 EiB");
    assert_repr<Bytes::bytes>(0x1p80F, "1048576 EiB");
    assert_repr<Bytes::bytes>(-0x1.8p80L, "-1572864 EiB");
}

TEST_CASE("ByteUnit::write takes any arithmetic type") {
    assert_repr<Bytes::bytes>(u8{255}, "255 B");
    assert_repr<Bytes::kibibytes>(u8{255}, "255 KiB");
    assert_repr<Bytes::bytes>(u16{65535}, "63 KiB");
    assert_repr<Bytes::bytes>(i16{-32768}, "-32 KiB");
    assert_repr<Bytes::bytes>(u32{1536}, "1.5 KiB");
    assert_repr<Bytes::bytes>(std::numeric_limits<u32>::max(), "3.9 GiB");
    assert_repr<Bytes::bytes>(1536.0, "1.5 KiB");
    assert_repr<Bytes::bytes>(1536.0F, "1.5 KiB");
    assert_repr<Bytes::bytes>(0.5, "0.5 B");
    assert_repr<Bytes::bytes>(0.05, "0 B");
}

TEST_CASE("ByteUnit::write signs negative values") {
    assert_repr<Bytes::bytes>(i64{-1}, "-1 B");
    assert_repr<Bytes::bytes>(i32{-1536}, "-1.5 KiB");
    assert_repr<Bytes::bytes>(i32{-2047}, "-1.9 KiB");
    assert_repr<Bytes::bytes>(-1536.0, "-1.5 KiB");
    assert_repr<Bytes::bytes>(-0.0, "0 B");
}

TEST_CASE("ByteUnit::write rejects values it can't represent") {
    assert_repr<Bytes::bytes>(
        std::numeric_limits<f64>::quiet_NaN(),
        UnitReprError::OutOfRange
    );
    assert_repr<Bytes::bytes>(
        std::numeric_limits<f32>::infinity(),
        UnitReprError::OutOfRange
    );
}

TEST_CASE("ByteUnit::write returns how many bytes it wrote") {
    auto buf = unwrap(Slice<u8>::make(ByteUnit::REPR_MAX_LEN<u64>));
    const usize n = unwrap(ByteUnit::write(u64{1536}, Bytes::bytes, *buf));
    CHECK(n == 7);
    CHECK(buf->first(n) == "1.5 KiB");
}

TEST_CASE("ByteUnit::write reports a short buffer at every piece") {
    for (usize cap = 0; cap < 8; ++cap)
        assert_repr<Bytes::bytes>(
            i64{-1536},
            UnitReprError::BufferTooSmall,
            cap
        );
    assert_repr<Bytes::bytes>(i64{-1536}, "-1.5 KiB", 8);
    assert_repr<Bytes::bytes>(u64{1}, "1 B", 3);
    assert_repr<Bytes::bytes>(u64{1}, UnitReprError::BufferTooSmall, 2);
}

TEST_CASE("ByteUnit::REPR_MAX_LEN fits the widest value of each type") {
    CHECK(ByteUnit::SYMBOL_MAX_LEN == 3);
    check_widest_every_unit<ByteUnit>();
}

TEST_CASE("ByteUnit::ratio divides the scales of two units") {
    CHECK(ByteUnit::ratio(Bytes::bytes, Bytes::bytes) == 1);
    CHECK(ByteUnit::ratio(Bytes::bytes, Bytes::kibibytes) == 1024);
    CHECK(ByteUnit::ratio(Bytes::kibibytes, Bytes::exbibytes) == u64{1} << 50);
    CHECK(ByteUnit::ratio(Bytes::bytes, Bytes::exbibytes) == u64{1} << 60);
}
