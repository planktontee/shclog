#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "doctest.h"
#include <sys/mman.h>

#include "shclog/io/mmap.hpp"

using namespace shclog::io::mmap;

TEST_CASE("Basic mmap/munmap") {
    constexpr size_t target_len = 4096 / sizeof(int);
    auto r = mmap<int>(target_len);
    REQUIRE(r.has_value());

    auto ptr = std::move(r.value());
    CHECK(ptr.get() != nullptr);

    // this aborts on failure in debug
    ptr.reset();

    CHECK(ptr.get() == nullptr);
}
