#include <string_view>
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "doctest.h"
#include "shclog/io/iouring.hpp"

using namespace shclog::io;
using namespace shclog::io::iouring;

TEST_CASE("Pwritev/read with ring") {
    auto ring_r =
        EventedIo::create(2, IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER);
    REQUIRE(ring_r.has_value());
    auto ring = std::move(ring_r.value());

    auto tmp_r = file::tmpfile();
    REQUIRE(tmp_r.has_value());
    auto tmp_fd = std::move(tmp_r.value());

    const iovec iovecs[2] = {
        {.iov_base = const_cast<char *>("hello "), .iov_len = 6},
        {.iov_base = const_cast<char *>("world!!!!\n"), .iov_len = 10},
    };

    auto writev_push_r =
        ring->push_writev(tmp_fd.get(), std::span{iovecs, 2}, 0);
    CHECK(writev_push_r == EventedIo::PushResult::Success);

    auto writev_pop_r = ring->pop_writev();
    REQUIRE(writev_pop_r.has_value());
    CHECK(writev_pop_r.value() == 16);

    uint8_t buf[12]{};

    auto read_push_r = ring->push_read(tmp_fd.get(), std::span{buf, 12}, 0);
    CHECK(read_push_r == EventedIo::PushResult::Success);

    auto read_pop_r = ring->pop_read();
    REQUIRE(read_pop_r.has_value());
    CHECK(read_pop_r.value() == 12);
    CHECK(std::string_view(reinterpret_cast<const char *>(buf),
                           read_pop_r.value()) == "hello world!");
}
