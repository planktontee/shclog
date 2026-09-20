#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include <sys/mman.h>

#include "shclog/io/mmap.hpp"
#include "shclog/types.hpp"

using namespace shclog;
using namespace shclog::io::mmap;

TEST_CASE("Basic mmap/munmap") {
    constexpr usize target_len = 4096 / sizeof(i32);
    auto r = mmap<i32>(target_len);
    REQUIRE(r.has_value());

    auto ptr = std::move(r.value());
    CHECK(ptr.get() != nullptr);

    // this aborts on failure in debug
    ptr.reset();

    CHECK(ptr.get() == nullptr);
}
