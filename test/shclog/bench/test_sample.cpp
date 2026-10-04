#include "shclog/doctest.hpp"

#include "shclog/bench/sample.hpp"
#include "shclog/cast.hpp"
#include "shclog/lang.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <concepts>
#include <initializer_list>
#include <limits>

using namespace shclog;
using namespace shclog::lang;
using namespace shclog::bench::sample;

TEST_CASE("Sample::avg reports an empty sample") {
    const auto samples = unwrap(Sample<u32>::make(4));

    const auto as_int = samples.avg<u64>();
    REQUIRE(!as_int.has_value());
    CHECK(as_int.error() == Sample<u32>::AvgError::EmptySamples);

    const auto as_float = samples.avg<f64>();
    REQUIRE(!as_float.has_value());
    CHECK(as_float.error() == Sample<u32>::AvgError::EmptySamples);
}

TEST_CASE("Sample::avg truncates for integer targets only") {
    auto samples = unwrap(Sample<u32>::make(8));
    for (const u32 v : {10U, 10U, 10U, 10U, 1000U})
        unwrap(samples.push(v));

    CHECK(unwrap(samples.avg<u64>()) == 208);
    CHECK(unwrap(samples.avg<f64>()) == 208.0);

    unwrap(samples.push(1));

    CHECK(unwrap(samples.avg<u64>()) == 173);
    CHECK(unwrap(samples.avg<f64>()) == 173.5);
}

TEST_CASE("Sample::avg of integer samples defaults to f64") {
    auto samples = unwrap(Sample<u32>::make(2));
    unwrap(samples.push(1));
    unwrap(samples.push(2));

    static_assert(std::same_as<decltype(unwrap(samples.avg())), f64>);
    CHECK(unwrap(samples.avg()) == 1.5);
}

TEST_CASE("Sample::avg recovers the mean once total has overflowed") {
    auto samples = unwrap(Sample<u32>::make(8));
    // this is ignored
    std::fill(
        samples.samples->begin(),
        samples.samples->end(),
        std::numeric_limits<u32>::max()
    );

    for (const u32 v : {4'000'000'000U, 4'000'000'000U, 4U})
        unwrap(samples.push(v));

    REQUIRE(unwrap(samples.total()).overflow);
    // 8'000'000'004 / 3
    CHECK(unwrap(samples.avg<u64>()) == 2'666'666'668);
    CHECK(unwrap(samples.avg<f64>()) == doctest::Approx(2'666'666'668.0));
}

TEST_CASE("Sample::avg recovers from a u64 overflow through f64") {
    constexpr u64 max = std::numeric_limits<u64>::max();
    auto samples = unwrap(Sample<u64>::make(2));
    unwrap(samples.push(max));
    unwrap(samples.push(max));

    REQUIRE(unwrap(samples.total()).overflow);
    CHECK(unwrap(samples.avg<f64>()) == float_cast<f64>(max));
}

TEST_CASE("Sample::avg of floating samples defaults to the sample type") {
    auto samples = unwrap(Sample<f64>::make(4));
    for (const f64 v : {0.5, 1.5, 4.0})
        unwrap(samples.push(v));

    CHECK(unwrap(samples.avg()) == 2.0);
}

TEST_CASE("Sample::avg recovers from a floating overflow in the same type") {
    auto samples = unwrap(Sample<f64>::make(2));
    unwrap(samples.push(1e308));
    unwrap(samples.push(1e308));

    REQUIRE(unwrap(samples.total()).overflow);
    CHECK(unwrap(samples.avg()) == 1e308);
}

TEST_CASE("Sample::avg keeps the sign of negative samples") {
    auto samples = unwrap(Sample<i32>::make(2));
    unwrap(samples.push(-3));
    unwrap(samples.push(-4));

    CHECK(unwrap(samples.avg<i64>()) == -3);
    CHECK(unwrap(samples.avg<f64>()) == -3.5);

    constexpr i32 min = std::numeric_limits<i32>::min();
    auto low = unwrap(Sample<i32>::make(2));
    unwrap(low.push(min));
    unwrap(low.push(min));

    REQUIRE(unwrap(low.total()).overflow);
    CHECK(unwrap(low.avg<i64>()) == min);
}

template <typename T> void check_invalid_percentiles() {
    auto samples = unwrap(Sample<T>::make(2));
    unwrap(samples.push(T{1}));
    unwrap(samples.push(T{2}));

    using L = std::numeric_limits<f64>;
    for (const f64 p :
         {L::quiet_NaN(), -L::infinity(), L::infinity(), -0.5, 1.5}) {
        CAPTURE(p);
        const auto r = samples.percentile(p);
        REQUIRE(!r.has_value());
        CHECK(r.error() == Sample<T>::PercentileError::InvalidPercentile);
    }
    CHECK(unwrap(samples.percentile(0.0)) == T{1});
    CHECK(unwrap(samples.percentile(1.0)) == T{2});
}

TEST_CASE("Sample::percentile rejects anything outside [0, 1], NaN included") {
    check_invalid_percentiles<u64>();
    check_invalid_percentiles<f64>();
}
