#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>

#include "shclog/bench/sample.hpp"
#include "shclog/bench/time.hpp"
#include "shclog/cast.hpp"
#include "shclog/types.hpp"
#include <array>
#include <chrono>
#include <utility>

using namespace shclog;
using namespace shclog::bench::sample;
using namespace shclog::bench::time;

namespace {

using Nano = std::chrono::nanoseconds;
using Micro = std::chrono::microseconds;
using Milli = std::chrono::milliseconds;
using Sec = std::chrono::seconds;

struct MockClock {
    using duration = Nano;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<MockClock, duration>;

    static constexpr bool is_steady = true;
    static constexpr duration epoch{1};

    static inline duration current = epoch;

    [[nodiscard]] static time_point now() noexcept {
        return time_point{current};
    }

    static void advance(const duration d) noexcept { current += d; }

    static void reset() noexcept { current = epoch; }
};

static_assert(Clock<MockClock>);

using Timer = Time<MockClock, Nano>;

template <typename T> [[nodiscard]] Sample<T> make_sample(const usize len) {
    auto r = Sample<T>::make(len);
    REQUIRE(r.has_value());
    return std::move(r.value());
}

struct DoubleTransform {
    [[nodiscard]] constexpr u64
    operator()(const Delta<MockClock, Nano> n) const noexcept {
        return int_cast<u64>(n.count()) * u64{2};
    }
};

static_assert(DurationTransform<DoubleTransform, u64, MockClock, Nano, Nano>);

template <typename T, typename TarD>
using Cast = CastTransformFn<T, MockClock, Nano, TarD>;

static_assert(DurationTransform<Cast<f64, Micro>, f64, MockClock, Nano, Micro>);
static_assert(DurationTransform<Cast<f32, Micro>, f32, MockClock, Nano, Micro>);

static_assert(Cast<f64, Micro>{}(Nano{1500}) == 1.5);
static_assert(Cast<f64, Milli>{}(Nano{1'500'000}) == 1.5);
static_assert(Cast<f64, Sec>{}(Nano{250'000'000}) == 0.25);
static_assert(Cast<u64, Micro>{}(Nano{1500}) == 1);

} // namespace

TEST_CASE("Time::sample records the elapsed delta in nanoseconds") {
    MockClock::reset();
    auto samples = make_sample<u64>(4);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{1500});
    CHECK(timer.sample(samples) == Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 1);
    CHECK(samples.span()[0] == 1500);
    CHECK(samples.min == 1500);
    CHECK(samples.max == 1500);
    CHECK(samples.total.value == 1500);
    CHECK(!samples.total.overflow);
}

TEST_CASE("Time::sample converts to microseconds, truncating toward zero") {
    MockClock::reset();
    auto samples = make_sample<u64>(4);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{1500});
    CHECK(timer.sample<u64, Micro>(samples) ==
          Sample<u64>::PushResult::Success);

    timer.start();
    MockClock::advance(Nano{2999});
    CHECK(timer.sample<u64, Micro>(samples) ==
          Sample<u64>::PushResult::Success);

    timer.start();
    MockClock::advance(Nano{3000});
    CHECK(timer.sample<u64, Micro>(samples) ==
          Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 3);
    CHECK(samples.span()[0] == 1);
    CHECK(samples.span()[1] == 2);
    CHECK(samples.span()[2] == 3);
}

TEST_CASE("Time::sample converts to milliseconds") {
    MockClock::reset();
    auto samples = make_sample<u64>(2);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{1'500'000});
    CHECK(timer.sample<u64, Milli>(samples) ==
          Sample<u64>::PushResult::Success);

    timer.start();
    MockClock::advance(Milli{4});
    CHECK(timer.sample<u64, Milli>(samples) ==
          Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 2);
    CHECK(samples.span()[0] == 1);
    CHECK(samples.span()[1] == 4);
}

TEST_CASE("Time::sample yields zero for a delta below the target unit") {
    MockClock::reset();
    auto samples = make_sample<u64>(2);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{999});
    CHECK(timer.sample<u64, Micro>(samples) ==
          Sample<u64>::PushResult::Success);

    timer.start();
    MockClock::advance(Nano{0});
    CHECK(timer.sample<u64, Micro>(samples) ==
          Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 2);
    CHECK(samples.span()[0] == 0);
    CHECK(samples.span()[1] == 0);
    CHECK(samples.min == 0);
    CHECK(samples.max == 0);
    CHECK(samples.total.value == 0);
}

TEST_CASE("Sample aggregates in the target unit") {
    MockClock::reset();
    auto samples = make_sample<u64>(4);
    Timer timer{};

    const std::array<Nano, 4> deltas{Nano{4000}, Nano{1000}, Nano{3000},
                                     Nano{2000}};
    for (const auto delta : deltas) {
        timer.start();
        MockClock::advance(delta);
        CHECK(timer.sample<u64, Micro>(samples) ==
              Sample<u64>::PushResult::Success);
    }

    CHECK(samples.count == 4);
    CHECK(samples.min == 1);
    CHECK(samples.max == 4);
    CHECK(samples.total.value == 10);
    CHECK(!samples.total.overflow);

    const auto p50 = samples.percentile(0.50);
    REQUIRE(p50.has_value());
    CHECK(p50.value() == 2);

    const auto p99 = samples.percentile(0.99);
    REQUIRE(p99.has_value());
    CHECK(p99.value() == 3);

    const auto p100 = samples.percentile(1.0);
    REQUIRE(p100.has_value());
    CHECK(p100.value() == 4);
}

TEST_CASE("Time::sample accepts a caller-supplied transform") {
    MockClock::reset();
    auto samples = make_sample<u64>(2);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{21});
    CHECK(timer.sample<u64>(samples, DoubleTransform{}) ==
          Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 1);
    CHECK(samples.span()[0] == 42);
}

TEST_CASE("Time measures the interval from start() to sample()") {
    MockClock::reset();
    auto samples = make_sample<u64>(2);
    Timer timer{};

    CHECK(!timer.started());

    MockClock::advance(Nano{10'000});
    timer.start();
    CHECK(timer.started());

    MockClock::advance(Nano{7});
    CHECK(timer.sample(samples) == Sample<u64>::PushResult::Success);
    CHECK(!timer.started());

    MockClock::advance(Nano{10'000});
    timer.start();
    MockClock::advance(Nano{9});
    CHECK(timer.sample(samples) == Sample<u64>::PushResult::Success);

    REQUIRE(samples.count == 2);
    CHECK(samples.span()[0] == 7);
    CHECK(samples.span()[1] == 9);

    MockClock::current = MockClock::duration::zero();
    Timer at_epoch{};

    at_epoch.start();
    CHECK(!at_epoch.started());

    MockClock::reset();
}

TEST_CASE("CastTransformFn keeps sub-unit precision for floating targets") {
    CHECK(Cast<f64, Nano>{}(Nano{1500}) == 1500.0);
    CHECK(Cast<f64, Micro>{}(Nano{1500}) == 1.5);
    CHECK(Cast<f64, Milli>{}(Nano{1'500'000}) == 1.5);
    CHECK(Cast<f64, Sec>{}(Nano{250'000'000}) == 0.25);

    CHECK(Cast<f64, Micro>{}(Nano{999}) == doctest::Approx(0.999));
    CHECK(Cast<f64, Sec>{}(Nano{1}) == doctest::Approx(1.0e-9));
    CHECK(Cast<f64, Micro>{}(Nano{0}) == 0.0);
}

TEST_CASE("CastTransformFn truncates for integer targets only") {
    CHECK(Cast<u64, Micro>{}(Nano{1500}) == 1);
    CHECK(Cast<f64, Micro>{}(Nano{1500}) == 1.5);

    CHECK(Cast<u64, Micro>{}(Nano{999}) == 0);
    CHECK(Cast<f64, Micro>{}(Nano{999}) > 0.0);

    CHECK(Cast<u64, Sec>{}(Nano{250'000'000}) == 0);
    CHECK(Cast<f64, Sec>{}(Nano{250'000'000}) == 0.25);
}

TEST_CASE("CastTransformFn converts to f32 targets") {
    CHECK(Cast<f32, Micro>{}(Nano{1500}) == 1.5F);
    CHECK(Cast<f32, Milli>{}(Nano{1'500'000}) == 1.5F);
    CHECK(Cast<f32, Micro>{}(Nano{999}) == doctest::Approx(0.999));
}

TEST_CASE("Time::sample reports a full sample buffer") {
    MockClock::reset();
    auto samples = make_sample<u64>(1);
    Timer timer{};

    timer.start();
    MockClock::advance(Nano{10});
    CHECK(timer.sample(samples) == Sample<u64>::PushResult::Success);

    timer.start();
    MockClock::advance(Nano{20});
    CHECK(timer.sample(samples) == Sample<u64>::PushResult::BufferFull);

    REQUIRE(samples.count == 1);
    CHECK(samples.span()[0] == 10);
    CHECK(samples.max == 10);
}
