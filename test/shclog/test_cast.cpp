#include "shclog/doctest.hpp"

#include "shclog/cast.hpp"
#include "shclog/types.hpp"

using namespace shclog;

static_assert(an_integer<u8> && an_integer<const i64>);
static_assert(!an_integer<bool> && !an_integer<const bool>);
static_assert(!an_integer<char> && !an_integer<const char>);
static_assert(!an_integer<volatile char8_t> && !an_integer<const wchar_t>);

TEST_CASE("int_cast to a const target is a plain cast") {
    CHECK(int_cast<const u8>(u8{200}) == 200);
    CHECK(int_cast<const u16>(u8{200}) == 200);
}
