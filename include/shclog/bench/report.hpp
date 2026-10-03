#pragma once

#include "shclog/cast.hpp"
#include "shclog/collections/slice.hpp"
#include "shclog/fmt.hpp"
#include "shclog/math.hpp"
#include <cmath>
#include <concepts>
#include <expected>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace shclog::bench::report {
using namespace shclog::collections::slice;
using namespace shclog::math;

enum class UnitReprError : u8 {
    Unexpected,
    BufferTooSmall,
    OutOfRange,
};

template <class U, class T>
concept UnitFmt =
    requires { typename U::Unit; } && std::is_scoped_enum_v<typename U::Unit> &&
    requires(const U::Unit u, const T value, Slice<u8> &buf) {
        { U::symbol(u) } noexcept -> std::same_as<std::u8string_view>;
        {
            U::write(value, u, buf)
        } noexcept -> std::same_as<std::expected<usize, UnitReprError>>;
    };

struct TimeUnit {
  public:
    enum class Unit : u8 {
        nanoseconds,
        microseconds,
        milliseconds,
        seconds,
        minutes,
        hours,
    };

    template <typename T>
        requires an_integer<T>
    static constexpr Unit from(const T idx) noexcept {
        assert(idx <= LAST_UNIT);
        return static_cast<Unit>(idx);
    }

    static constexpr usize step(const Unit u) noexcept {
        switch (u) {
        case Unit::nanoseconds:
            return 1;
        case Unit::microseconds:
        case Unit::milliseconds:
        case Unit::seconds:
            return 1000;
        case Unit::minutes:
        case Unit::hours:
            return 60;
        }
        std::unreachable();
    }

    static constexpr std::u8string_view symbol(const Unit u) noexcept {
        switch (u) {
        case Unit::nanoseconds:
            return u8"ns";
        case Unit::microseconds:
            // this is 3 bytes
            return u8"µs";
        case Unit::milliseconds:
            return u8"ms";
        case Unit::seconds:
            return u8"s";
        case Unit::minutes:
            return u8"m";
        case Unit::hours:
            return u8"h";
        }
        std::unreachable();
    }

    static constexpr usize LAST_UNIT = std::to_underlying(Unit::hours);

    static constexpr usize ratio(const Unit from, const Unit to) noexcept {
        assert(from <= to);
        usize r = 1;
        for (usize i = usize{std::to_underlying(from)} + 1;
             i <= std::to_underlying(to); ++i)
            r *= step(TimeUnit::from(i));
        return r;
    }

    // (-)? + (max repr size) + ' ' + (max unit size)
    template <Arithmetic T>
    static constexpr usize REPR_MAX_LEN = int_cast<usize>(
        (an_integer<T> ? std::numeric_limits<T>::digits10 + 1
                       : std::numeric_limits<T>::max_exponent10 + 1) +
        1 + 1 + 3);

    template <Arithmetic T>
    static constexpr Unit biggest_unit_fit(const T value,
                                           const Unit u) noexcept {
        usize idx = std::to_underlying(u);
        while (idx < LAST_UNIT && fills_unit(value, u, from(idx + 1)))
            ++idx;
        return static_cast<Unit>(idx);
    }

    template <Arithmetic T> struct ValueInUnit {
        T whole;
        u8 tenth;
        Unit unit;
    };

    template <Arithmetic T>
    [[nodiscard]] static std::expected<ValueInUnit<T>, UnitReprError>
    fit(const T value, const Unit u) noexcept {
        if constexpr (std::floating_point<T>) {
            if (!std::isfinite(value))
                return std::unexpected(UnitReprError::OutOfRange);
        }

        const Unit biggest_u = biggest_unit_fit(value, u);
        const usize unit_ratio = ratio(u, biggest_u);

        T whole{};
        u8 tenth = 0;
        if constexpr (an_integer<T>) {
            const T ratio_in_t = int_cast<T>(unit_ratio);
            whole = int_cast<T>(value / ratio_in_t);

            // unit_r being 1 means there's no diff in ratio between u and
            // biggest_u
            // we are using the following format:
            //    - x < 10 -> has decimals
            //    - max number of decimas is 1
            if (unit_ratio != 1 && std::cmp_less(whole, 10) &&
                std::cmp_greater(whole, -10)) {
                auto rem = value % ratio_in_t;

                if constexpr (std::is_signed_v<T>)
                    rem = rem < 0 ? -rem : rem;

                // from reminder we get the first / 10 to get a
                // single digit
                tenth = int_cast<u8>(rem / (ratio_in_t / 10));
            }
        } else {
            const T ratio_in_t = float_cast<T>(unit_ratio);
            const T q = value / ratio_in_t;
            if (std::abs(q) < T{10}) {
                const T tenths = std::trunc(value * T{10} / ratio_in_t);
                whole = std::trunc(tenths / T{10});
                if (std::abs(whole) < T{10})
                    tenth = int_cast<u8>(std::abs(tenths - whole * T{10}));
            } else {
                whole = std::trunc(q);
            }
        }
        return ValueInUnit<T>{whole, tenth, biggest_u};
    }

    template <Arithmetic T>
    [[nodiscard]] static std::expected<usize, UnitReprError>
    write(const T value, const Unit u, Slice<u8> &buf) noexcept {
        const auto r = fit(value, u);
        if (!r.has_value())
            return std::unexpected(r.error());
        return write(*r, buf);
    }

    template <Arithmetic T>
    [[nodiscard]] static std::expected<usize, UnitReprError>
    write(const ValueInUnit<T> &v, Slice<u8> &buf) noexcept {
        T whole = v.whole;

        // -0 if valid in fXX
        if (v.tenth == 0 && whole == T{0})
            whole = T{0};

        u8 *const first = buf.data;
        u8 *const last = first + buf.len;
        u8 *it = first;
        const auto put = [&](const u8 c) {
            if (it == last) [[unlikely]]
                return false;
            *it++ = c;
            return true;
        };

        const auto number = write_number(buf, whole);
        if (!number.has_value())
            return std::unexpected(UnitReprError::BufferTooSmall);

        it += *number;
        if (v.tenth != 0) {
            if (!put('.')) [[unlikely]]
                return std::unexpected(UnitReprError::BufferTooSmall);

            assert(v.tenth < 10);
            if (!put(int_cast<u8>('0' + v.tenth))) [[unlikely]]
                return std::unexpected(UnitReprError::BufferTooSmall);
        }

        if (!put(' ')) [[unlikely]]
            return std::unexpected(UnitReprError::BufferTooSmall);

        for (const u8 c : symbol(v.unit))
            if (!put(c)) [[unlikely]]
                return std::unexpected(UnitReprError::BufferTooSmall);

        return int_cast<usize>(it - first);
    }

  private:
    template <Arithmetic T>
    static constexpr bool fills_unit(const T v, const Unit from,
                                     const Unit to) noexcept {
        const auto r = ratio(from, to);
        if constexpr (an_integer<T>)
            return std::cmp_greater_equal(v, r) ||
                   std::cmp_less_equal(v, -int_cast<isize>(r));
        else
            return std::abs(v) >= float_cast<T>(r);
    }
};

} // namespace shclog::bench::report
