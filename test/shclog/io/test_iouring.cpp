#include "shclog/io/syscall.hpp"
#include <algorithm>
#include <array>
#include <asm/unistd_64.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <ios>
#include <iostream>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <linux/openat2.h>
#include <memory>
#include <new>
#include <numeric>
#include <pthread.h>
#include <random>
#include <ranges>
#include <sched.h>
#include <set>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "doctest.h"
#include "shclog/io/cpu.hpp"
#include "shclog/io/iouring.hpp"
#include "shclog/io/process.hpp"
#include "shclog/mpsc_queue.hpp"

using namespace shclog::io;
using namespace shclog::io::iouring;
using namespace shclog::io::cpu;
using namespace shclog::io::process;
using namespace shclog::mpsc_queue;

TEST_CASE("Pwritev/read with ring") {
    auto evented_r =
        EventedIo::create(2, IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER);
    REQUIRE(evented_r.has_value());
    auto evented = std::move(evented_r.value());

    auto tmp_r = file::tmpfile();
    REQUIRE(tmp_r.has_value());
    auto tmp_fd = std::move(tmp_r.value());

    const fd_t target_fds[1]{tmp_fd.get()};
    CHECK(evented->register_files(std::span{target_fds, 1}));

    tmp_fd.release();
    const fd_t tmp_fd_idx = 0;

    char w_buf_1[] = "hello world!!!!\n";
    uint8_t r_buf_1[12]{};
    const iovec buffers[2] = {
        {.iov_base = w_buf_1, .iov_len = 16},
        {.iov_base = r_buf_1, .iov_len = 12},
    };

    const iovec iovecs[2] = {
        {.iov_base = w_buf_1, .iov_len = 6},
        {.iov_base = w_buf_1 + 6, .iov_len = 10},
    };
    CHECK(evented->register_buffers(std::span{buffers, 2}));

    auto writev_push_r = evented->push_writev(tmp_fd_idx, std::span{iovecs, 2},
                                              0, IOSQE_FIXED_FILE, true, 0);
    CHECK(writev_push_r == EventedIo::PushResult::Success);

    auto writev_pop_r = evented->pop_writev();
    REQUIRE(writev_pop_r.has_value());
    CHECK(writev_pop_r.value() == 16);

    auto read_push_r = evented->push_read(tmp_fd_idx, std::span{r_buf_1, 12}, 0,
                                          IOSQE_FIXED_FILE, true, 1);
    CHECK(read_push_r == EventedIo::PushResult::Success);

    auto read_pop_r = evented->pop_read();
    REQUIRE(read_pop_r.has_value());
    CHECK(read_pop_r.value() == 12);
    CHECK(std::string_view(reinterpret_cast<const char *>(r_buf_1),
                           read_pop_r.value()) == "hello world!");
}

#define RUN_CHECKS 1
#define BENCHMARK 0
#define QUEUE_TYPE 2

TEST_CASE("MPSC -> dual-buffer/memcpy WRITE") {
    using clock = std::chrono::steady_clock;

#if BENCHMARK
    auto cpu_cores_r = list_cpu_cores();
    CHECK(cpu_cores_r.has_value());
    const auto &cores = cpu_cores_r.value();

    const std::set<size_t> target_cores = [&] {
        std::set<size_t> shadow;
        for (const auto &core : cores)
            shadow.insert(core.siblings[0]);
        if (shadow.size() == 0)
            shadow.insert(0);
        return shadow;
    }();

    // MAX - main - iouring
    const size_t PRODUCERS =
        std::max(static_cast<size_t>(1),
                 std::sub_sat(target_cores.size(), static_cast<size_t>(2)));
    // this should be a ratio for MAX_HW_CORES
    constexpr size_t LINES_PER_PRODUCER = (1 << 10) * 512;
#if QUEUE_TYPE != 2
    constexpr size_t QUEUE_CAPACITY = (1 << 10) * 32;
#endif
    constexpr size_t MAX_IO_BYTES = (1 << 10) * 128;
    constexpr size_t MIN_LEN = 128;
    constexpr size_t MAX_LINE_LEN = (1 << 10) * 2;
#else
    constexpr size_t PRODUCERS = 3;
    constexpr size_t LINES_PER_PRODUCER = (1 << 5);
#if QUEUE_TYPE != 2
    constexpr size_t QUEUE_CAPACITY = 512;
#endif
    constexpr size_t MAX_IO_BYTES = (1 << 10);
    constexpr size_t MIN_LEN = 64;
    constexpr size_t MAX_LINE_LEN = 256;
#endif
    const size_t TOTAL_LINES = PRODUCERS * LINES_PER_PRODUCER;
    constexpr size_t IO_BUFFERS = 2;
    constexpr auto TARGET_FLUSH =
        std::chrono::nanoseconds(static_cast<uint64_t>(1e9 / 30));
    constexpr size_t PREALLOC_CHUNK = MAX_IO_BYTES << 8;
    constexpr size_t ALLOC_WATERMARK = MAX_IO_BYTES << 1;

    struct Message {
#if QUEUE_TYPE == 2
        Node node;
#endif
        uint8_t *data;
        const size_t size;

        Message(uint8_t *data, const size_t size) noexcept
            :
#if QUEUE_TYPE == 2
              node(nullptr),
#endif
              data(data), size(size) {};

        std::span<uint8_t> bytes() noexcept { return {data, size}; }

        std::span<const uint8_t> bytes() const noexcept { return {data, size}; }
    };

    struct ByteArrDeleter {
        void operator()(void *const ptr) const {
            if (ptr)
                delete[] reinterpret_cast<uint8_t *>(ptr);
        }
    };

#if BENCHMARK == 1
    struct Stats {
        uint64_t falloc_total_ns = 0;
        uint64_t falloc_min_ns = UINT64_MAX;
        uint64_t falloc_max_ns = 0;

        uint64_t queue_total_ns = 0;
        uint64_t queue_min_ns = UINT64_MAX;
        uint64_t queue_max_ns = 0;

        uint64_t memcpy_total_ns = 0;
        uint64_t memcpy_min_ns = UINT64_MAX;
        uint64_t memcpy_max_ns = 0;

        uint64_t free_total_ns = 0;
        uint64_t free_min_ns = UINT64_MAX;
        uint64_t free_max_ns = 0;

        uint64_t push_io_total_ns = 0;
        uint64_t push_io_min_ns = UINT64_MAX;
        uint64_t push_io_max_ns = 0;

        uint64_t pop_io_total_ns = 0;
        uint64_t pop_io_min_ns = UINT64_MAX;
        uint64_t pop_io_max_ns = 0;

        std::vector<uint64_t> msg_alloc_samples;
        std::vector<uint64_t> falloc_samples;
        std::vector<uint64_t> free_samples;
        std::vector<uint64_t> queue_samples;
        std::vector<uint64_t> memcpy_samples;
        std::vector<uint64_t> push_io_samples;
        std::vector<uint64_t> pop_io_samples;

        uint64_t bytes = 0;

        uint64_t writes = 0;
        uint64_t total_batch_lines = 0;
        uint64_t max_batch_lines = 0;

        std::vector<std::vector<uint64_t>> producer_enqueue_samples;

        Stats(const size_t consumer_capacity, const size_t producers_n,
              const size_t producer_capacity, const size_t max_line_len) {

            falloc_samples.reserve(max_line_len * consumer_capacity /
                                   ALLOC_WATERMARK);

            queue_samples.reserve(consumer_capacity);
            memcpy_samples.reserve(consumer_capacity);
            free_samples.reserve(consumer_capacity);
            push_io_samples.reserve(consumer_capacity);
            pop_io_samples.reserve(consumer_capacity);

            producer_enqueue_samples =
                std::vector<std::vector<uint64_t>>(producers_n);
            for (auto samples : producer_enqueue_samples)
                samples.reserve(producer_capacity);
        }
    };

    Stats stats(TOTAL_LINES, PRODUCERS, LINES_PER_PRODUCER, MAX_LINE_LEN);
#elif BENCHMARK == 2
    uint64_t byte_count = 0;
#endif

    const auto make_line =
        [](std::mt19937_64 &rng) -> std::unique_ptr<Message, ByteArrDeleter> {
        std::uniform_int_distribution<size_t> length_dist(MIN_LEN,
                                                          MAX_LINE_LEN);

        std::uniform_int_distribution<int> character_dist(32, 126);

        const size_t len = length_dist(rng);
        auto *const mem = new (std::nothrow) uint8_t[sizeof(Message) + len];
        if (!mem) [[unlikely]]
            std::unreachable();

        auto *const message = new (mem) Message(mem + sizeof(Message), len);

        for (size_t i = 0; i + 1 < len; ++i)
            message->data[i] = static_cast<char>(character_dist(rng));
        message->data[len - 1] = '\n';

        return std::unique_ptr<Message, ByteArrDeleter>(message);
    };

    auto evented_r = EventedIo::create(1, IORING_SETUP_SINGLE_ISSUER);
#if RUN_CHECKS
    REQUIRE(evented_r.has_value());
#endif
    auto evented = std::move(evented_r.value());

    auto tmp_r = file::tmpfile();
#if RUN_CHECKS
    REQUIRE(tmp_r.has_value());
#endif
    auto tmp_fd = std::move(tmp_r.value());
    const fd_t target_fds[1] = {tmp_fd.get()};
#if RUN_CHECKS
    const auto reg_fd_r =
#endif
        evented->register_files(std::span{target_fds, 1});
#if RUN_CHECKS
    CHECK(reg_fd_r);
#endif
    constexpr fd_t tmp_fd_idx = 0;

#if QUEUE_TYPE == 1
    MPSCQSlotted<Message, QUEUE_CAPACITY, ByteArrDeleter> queue{};
#elif QUEUE_TYPE == 2
    UnboundedLinkedMPSCQ<Message, ByteArrDeleter> queue{};
#else
    auto queue_r = MPSCQueue<Message, ByteArrDeleter>::create(QUEUE_CAPACITY);
#if RUN_CHECKS
    REQUIRE(queue_r.has_value());
#endif
    auto queue = std::move(queue_r.value());
#endif

#if RUN_CHECKS
    std::vector<uint8_t> expected;
    expected.reserve(TOTAL_LINES * MAX_LINE_LEN);
#endif

    std::atomic<size_t> producers_finished = 0;
    std::vector<std::jthread> producers;
    producers.reserve(PRODUCERS);
#if BENCHMARK
    CHECK(SetPriorityResult::Success == set_priority(-20));
    auto cpu_view =
        std::views::iota(*target_cores.begin(), *target_cores.rbegin() + 1) |
        std::views::filter(
            [&target_cores](size_t x) { return target_cores.contains(x); });
    CHECK(SetCpuAffinityResult::Success == set_cpu_afinity(cpu_view));

    REQUIRE(target_cores.size() >= 1);
    const std::vector<size_t> core_arr(target_cores.begin(),
                                       target_cores.end());

    const size_t main_cpu = static_cast<size_t>(sched_getcpu());
    const auto pick_cpu = [main_cpu, &core_arr](const size_t target) -> size_t {
        const auto idx = target % core_arr.size();
        if (core_arr[idx] >= main_cpu)
            return core_arr[(idx + 1) % core_arr.size()];
        else
            return core_arr[idx];
    };
#endif

    std::barrier ready(PRODUCERS + 1);
    for (size_t producer_id = 0; producer_id < PRODUCERS; ++producer_id) {
        producers.emplace_back([&, producer_id] {
#if BENCHMARK
            CHECK(SetCpuAffinityResult::Success ==
                  set_cpu_afinity(pick_cpu(producer_id)));
            CHECK(SetPriorityResult::Success == set_priority(-20));
#endif
            ready.arrive_and_wait();
            std::mt19937_64 rng(0x8f3a21c7d94e6b5ULL + producer_id);

            for (size_t line = 0; line < LINES_PER_PRODUCER; ++line) {
                auto message = make_line(rng);
#if BENCHMARK == 1
                const auto enqueue_start = clock::now();
#endif
#if QUEUE_TYPE == 1
                while (!queue.enqueue(std::move(message))) {
                    continue;
                }
#elif QUEUE_TYPE == 2
                queue.enqueue(std::move(message));
#else
                while (!queue->enqueue(std::move(message))) {
                    continue;
                }
#endif
#if BENCHMARK == 1
                const auto enqueue_completed = clock::now();
                const uint64_t enqueue_latency = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        enqueue_completed - enqueue_start)
                        .count());

                stats.producer_enqueue_samples[producer_id].push_back(
                    enqueue_latency);
#endif
            }

            producers_finished.fetch_add(1, std::memory_order_release);
        });
    }

    alignas(4096) std::array<std::array<uint8_t, MAX_IO_BYTES>, IO_BUFFERS>
        buffers;

    const iovec reg_buf[2] = {
        {.iov_base = buffers[0].data(), .iov_len = MAX_IO_BYTES},
        {.iov_base = buffers[1].data(), .iov_len = MAX_IO_BYTES},
    };
#if RUN_CHECKS
    const auto reg_bufs_r =
#endif
        evented->register_buffers(std::span{reg_buf, 2});
#if RUN_CHECKS
    CHECK(reg_bufs_r);
#endif

    std::array<size_t, IO_BUFFERS> line_count{};
    std::array<uint64_t, IO_BUFFERS> io_bytes{};
    std::array<bool, IO_BUFFERS> in_flight{};

    uint64_t file_offset = 0;
    size_t active_buffer = 0;
    size_t consumed_lines = 0;
    std::unique_ptr<Message, ByteArrDeleter> overflow = nullptr;
    size_t last_expand = 0;

    ready.arrive_and_wait();
#if BENCHMARK
    const auto benchmark_start = clock::now();
#endif
    while (consumed_lines < TOTAL_LINES) {
#if RUN_CHECKS
        CHECK(!in_flight[active_buffer]);
#endif

        line_count[active_buffer] = 0;
        io_bytes[active_buffer] = 0;

        auto buffer_iter = buffers[active_buffer].begin();
        const auto collection_start = clock::now();

        if (last_expand - file_offset < ALLOC_WATERMARK) {
#if BENCHMARK == 1
            const auto falloc_start = clock::now();
#endif
#if RUN_CHECKS == 1
            auto rc =
#endif
                fallocate(tmp_fd.get(), FALLOC_FL_KEEP_SIZE, last_expand,
                          PREALLOC_CHUNK);
#if BENCHMARK == 1
            const auto falloc_end = clock::now();
            const uint64_t falloc_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    falloc_end - falloc_start)
                    .count());

            stats.falloc_total_ns += falloc_ns;
            stats.falloc_min_ns = std::min(stats.falloc_min_ns, falloc_ns);
            stats.falloc_max_ns = std::max(stats.falloc_max_ns, falloc_ns);
            stats.falloc_samples.push_back(falloc_ns);
#endif
#if RUN_CHECKS == 1
            CHECK(rc == 0);
#endif
            last_expand += PREALLOC_CHUNK;
        }

        while ((clock::now() - collection_start) < TARGET_FLUSH &&
               io_bytes[active_buffer] < MAX_IO_BYTES &&
               consumed_lines < TOTAL_LINES) {

#if BENCHMARK == 1
            const auto queue_start = clock::now();
#endif
            std::unique_ptr<Message, ByteArrDeleter> message;
            if (!overflow.get()) [[likely]]
#if QUEUE_TYPE == 1
                message = queue.dequeue();
#elif QUEUE_TYPE == 2
                message = queue.dequeue();
#else
                message = queue->dequeue();
#endif
            else
                message = std::move(overflow);
#if BENCHMARK == 1
            const auto queue_end = clock::now();
            const uint64_t queue_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    queue_end - queue_start)
                    .count());

            stats.queue_total_ns += queue_ns;
            stats.queue_min_ns = std::min(stats.queue_min_ns, queue_ns);
            stats.queue_max_ns = std::max(stats.queue_max_ns, queue_ns);
            stats.queue_samples.push_back(queue_ns);
#endif

            if (!message) [[unlikely]] {
                if (producers_finished.load(std::memory_order_acquire) ==
                    PRODUCERS)
                    break;

                continue;
            }

            const auto data_span = message.get()->bytes();

            if (io_bytes[active_buffer] + data_span.size() >=
                buffers[active_buffer].size()) [[unlikely]] {
                overflow = std::move(message);
                break;
            } else {
#if RUN_CHECKS
                expected.insert(expected.end(), data_span.begin(),
                                data_span.end());
#endif
                io_bytes[active_buffer] += data_span.size();
#if BENCHMARK == 1
                const auto memcpy_start = clock::now();
#endif
                std::memcpy(buffer_iter, data_span.data(), data_span.size());
#if BENCHMARK == 1
                const auto memcpy_end = clock::now();
                const uint64_t memcpy_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        memcpy_end - memcpy_start)
                        .count());

                stats.memcpy_total_ns += memcpy_ns;
                stats.memcpy_min_ns = std::min(stats.memcpy_min_ns, memcpy_ns);
                stats.memcpy_max_ns = std::max(stats.memcpy_max_ns, memcpy_ns);
                stats.memcpy_samples.push_back(memcpy_ns);

#elif BENCHMARK == 2
                byte_count += data_span.size();
#endif
                buffer_iter += data_span.size();
                ++line_count[active_buffer];
                ++consumed_lines;

#if BENCHMARK == 1
                const auto free_start = clock::now();
#endif
                message.reset();
#if BENCHMARK == 1
                const auto free_end = clock::now();
                const uint64_t free_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        free_end - free_start)
                        .count());

                stats.free_total_ns += free_ns;
                stats.free_min_ns = std::min(stats.free_min_ns, free_ns);
                stats.free_max_ns = std::max(stats.free_max_ns, free_ns);
                stats.free_samples.push_back(free_ns);
#endif
            }
        }

        // this can only happen after produces finish/break
        if (line_count[active_buffer] == 0) [[unlikely]] {
            if (producers_finished.load(std::memory_order_acquire) == PRODUCERS)
                break;
            continue;
        }

        const auto in_flight_buffer = active_buffer ^ 1;
        if (in_flight[in_flight_buffer]) {
#if BENCHMARK == 1
            const auto pop_start = clock::now();
#endif
#if RUN_CHECKS
            const auto pop_r =
#endif
                evented->pop_write();
#if BENCHMARK == 1
            const auto pop_completed = clock::now();
            const uint64_t pop_latency = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    pop_completed - pop_start)
                    .count());
            stats.pop_io_total_ns += pop_latency;
            stats.pop_io_min_ns = std::min(stats.pop_io_min_ns, pop_latency);
            stats.pop_io_max_ns = std::max(stats.pop_io_max_ns, pop_latency);
            stats.pop_io_samples.push_back(pop_latency);
#endif

#if RUN_CHECKS
            CHECK(pop_r.value() ==
                  static_cast<int32_t>(io_bytes[in_flight_buffer]));
#endif
            in_flight[in_flight_buffer] = false;
        }

#if BENCHMARK == 1
        const auto push_start = clock::now();
#endif
#if RUN_CHECKS
        const auto push_r =
#endif
            evented->push_write(
                tmp_fd_idx,
                std::span<const uint8_t>{buffers[active_buffer].data(),
                                         io_bytes[active_buffer]},
                file_offset, IOSQE_FIXED_FILE, 0, true, active_buffer);
#if BENCHMARK == 1
        const auto push_completed = clock::now();
        const uint64_t push_latency = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                push_completed - push_start)
                .count());
        stats.push_io_total_ns += push_latency;
        stats.push_io_min_ns = std::min(stats.push_io_min_ns, push_latency);
        stats.push_io_max_ns = std::max(stats.push_io_max_ns, push_latency);
        stats.push_io_samples.push_back(push_latency);
#endif
#if RUN_CHECKS
        CHECK(push_r == EventedIo::PushResult::Success);
#endif

#if BENCHMARK == 1
        ++stats.writes;
        stats.max_batch_lines =
            std::max(stats.max_batch_lines,
                     static_cast<uint64_t>(line_count[active_buffer]));
        stats.bytes += io_bytes[active_buffer];
#endif

        file_offset += io_bytes[active_buffer];
        in_flight[active_buffer] = true;
        active_buffer ^= 1;
    }

#if RUN_CHECKS
    REQUIRE(producers_finished.load(std::memory_order_acquire) == PRODUCERS);
    REQUIRE(consumed_lines == TOTAL_LINES);
#endif

    // we get out of the loop for consumer once all producers are done
    for (size_t buffer = 0; buffer < IO_BUFFERS; ++buffer) {
        if (!in_flight[buffer])
            continue;

#if BENCHMARK == 1
        const auto pop_start = clock::now();
#endif

#if RUN_CHECKS
        const auto pop_r =
#endif
            evented->pop_write();

#if BENCHMARK == 1
        const auto completed = clock::now();
        const uint64_t pop_latency = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed -
                                                                 pop_start)
                .count());
        stats.pop_io_total_ns += pop_latency;
        stats.pop_io_min_ns = std::min(stats.pop_io_min_ns, pop_latency);
        stats.pop_io_max_ns = std::max(stats.pop_io_max_ns, pop_latency);
        stats.pop_io_samples.push_back(pop_latency);
#endif

#if RUN_CHECKS
        REQUIRE(pop_r.has_value());
        CHECK(pop_r.value() == static_cast<int32_t>(io_bytes[buffer]));
#endif

        in_flight[buffer] = false;
    }

#if BENCHMARK
    const auto benchmark_completed = clock::now();
#endif

    for (auto &t : producers)
        if (t.joinable())
            t.join();

#if RUN_CHECKS
    std::vector<uint8_t> actual(expected.size());
    size_t read_offset = 0;
    while (read_offset < actual.size()) {
        const size_t remaining = actual.size() - read_offset;

        const size_t chunk = std::min(remaining, MAX_IO_BYTES);
        auto push_r = evented->push_read(
            tmp_fd_idx, std::span{actual.data() + read_offset, chunk},
            static_cast<off_t>(read_offset), IOSQE_FIXED_FILE);

        CHECK(push_r == EventedIo::PushResult::Success);

        const auto pop_r = evented->pop_read();

        REQUIRE(pop_r.has_value());
        CHECK(pop_r.value() == static_cast<int32_t>(chunk));

        read_offset += chunk;
    }

    CHECK(read_offset == expected.size());

    size_t mismatch = expected.size();
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            mismatch = i;
            break;
        }
    }

    if (mismatch != expected.size()) {
        MESSAGE("first mismatch at byte "
                << mismatch
                << " expected=" << static_cast<unsigned>(expected[mismatch])
                << " actual=" << static_cast<unsigned>(actual[mismatch]));

        CHECK(mismatch == expected.size());
    }
#endif

#if BENCHMARK
    const auto elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            benchmark_completed - benchmark_start)
            .count());

    const double elapsed_seconds = static_cast<double>(elapsed_ns) / 1.0e9L;

    const double lines_per_second =
        static_cast<double>(consumed_lines) / elapsed_seconds;

#if BENCHMARK == 1
    REQUIRE(!stats.pop_io_samples.empty());
    REQUIRE(!stats.queue_samples.empty());

    const auto percentile = [](std::vector<uint64_t> values,
                               const double p) -> uint64_t {
#if RUN_CHECKS
        CHECK(!values.empty());
#endif

        const size_t index =
            static_cast<size_t>(p * static_cast<double>(values.size() - 1));

        return values[index];
    };

    const double mib_per_second =
        static_cast<double>(stats.bytes) / elapsed_seconds / (1 << 20);

    const double avg_falloc_ns =
        static_cast<double>(stats.falloc_total_ns) /
        static_cast<double>(stats.falloc_samples.size());

    const double avg_push_io_ns =
        static_cast<double>(stats.push_io_total_ns) /
        static_cast<double>(stats.push_io_samples.size());
    const double avg_pop_io_ns =
        static_cast<double>(stats.pop_io_total_ns) /
        static_cast<double>(stats.pop_io_samples.size());

    const double avg_queue_ns = static_cast<double>(stats.queue_total_ns) /
                                static_cast<double>(stats.queue_samples.size());

    const double avg_memcpy_ns =
        static_cast<double>(stats.memcpy_total_ns) /
        static_cast<double>(stats.memcpy_samples.size());

    const double avg_free_ns = static_cast<double>(stats.free_total_ns) /
                               static_cast<double>(stats.free_samples.size());

    const double avg_batch_lines =
        static_cast<double>(consumed_lines) / static_cast<double>(stats.writes);

    const double avg_write_bytes =
        static_cast<double>(stats.bytes) / static_cast<double>(stats.writes);

    std::ostringstream report;

    const auto reduce_as_micros =
        [](const std::vector<uint64_t> samples) -> uint64_t {
        return std::transform_reduce(
            samples.begin(), samples.end(), 0.0L, std::plus<>{},
            [](const uint64_t ns) { return static_cast<double>(ns) / 1.0e3L; });
    };

    std::sort(stats.falloc_samples.begin(), stats.falloc_samples.end());
    std::sort(stats.queue_samples.begin(), stats.queue_samples.end());
    std::sort(stats.memcpy_samples.begin(), stats.memcpy_samples.end());
    std::sort(stats.free_samples.begin(), stats.free_samples.end());
    std::sort(stats.pop_io_samples.begin(), stats.pop_io_samples.end());
    std::sort(stats.push_io_samples.begin(), stats.push_io_samples.end());

    uint64_t enqueue_max_ns = 0;
    uint64_t enqueue_min_ns = UINT64_MAX;
    std::vector<uint64_t> enqueue_samples;
    enqueue_samples.reserve(LINES_PER_PRODUCER);
    for (auto &samples : stats.producer_enqueue_samples) {
        enqueue_max_ns = std::max(enqueue_max_ns, std::ranges::max(samples));
        enqueue_min_ns = std::min(enqueue_min_ns, std::ranges::min(samples));
        enqueue_samples.insert(enqueue_samples.end(),
                               std::make_move_iterator(samples.begin()),
                               std::make_move_iterator(samples.end()));
    }
    const double avg_enqueue_ns = std::transform_reduce(
        enqueue_samples.begin(), enqueue_samples.end(), 0.0,
        [](const double a, const double b) { return a + b; },
        [&enqueue_samples](const auto ns) {
            return static_cast<double>(ns) / enqueue_samples.size();
        });
    std::sort(enqueue_samples.begin(), enqueue_samples.end());

    report << "\n"
           << std::fixed << std::setprecision(2)
           << "MPSC -> dual-buffer WRITEV\n"
           << "==========================\n"
           << "producers\t" << PRODUCERS << "\n"
           << "lines\t\t" << consumed_lines << "\n"
           << "bytes\t\t" << stats.bytes * (1.0L / (1 << 30)) << " GiB\n"
           << "\n"

           << "throughput\n"
           << "----------\n"
           << "elapsed\t\t" << elapsed_ns / 1.0e9L << " s\n"
           << "lines/sec\t" << lines_per_second << "\n"
           << "MiB/sec\t\t" << mib_per_second << "\n"
           << "\n"

           << "batching\n"
           << "--------\n"
           << "writes\t\t" << stats.writes << "\n"
           << "avg lines\t" << avg_batch_lines << "\n"
           << "max lines\t" << stats.max_batch_lines << "\n"
           << "avg bytes\t" << avg_write_bytes / 1024.0L << " KiB\n"
           << "\n"

           << "falloc latency\n"
           << "-------------\n"
           << "sum\t\t\t" << reduce_as_micros(stats.falloc_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t\t" << avg_falloc_ns << " ns\n"
           << "min\t\t\t" << stats.falloc_min_ns << " ns\n"
           << "p50\t\t\t" << percentile(stats.falloc_samples, 0.50) << " ns\n"
           << "p90\t\t\t" << percentile(stats.falloc_samples, 0.90) << " ns\n"
           << "p99\t\t\t" << percentile(stats.falloc_samples, 0.99) << " ns\n"
           << "p99.9\t\t\t" << percentile(stats.falloc_samples, 0.999) / 1.0e6L
           << " ms\n"
           << "p99.99\t\t\t"
           << percentile(stats.falloc_samples, 0.9999) / 1.0e6L << " ms\n"
           << "p99.999\t\t\t"
           << percentile(stats.falloc_samples, 0.99999) / 1.0e6L << " ms\n"
           << "max\t\t\t" << stats.falloc_max_ns / 1.0e6L << " ms\n"
           << "\n"

           << "enqueue latency\n"
           << "-------------\n"
           << "sum\t\t\t" << reduce_as_micros(enqueue_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t\t" << avg_enqueue_ns << " ns\n"
           << "min\t\t\t" << enqueue_min_ns << " ns\n"
           << "p50\t\t\t" << percentile(enqueue_samples, 0.50) << " ns\n"
           << "p90\t\t\t" << percentile(enqueue_samples, 0.90) << " ns\n"
           << "p99\t\t\t" << percentile(enqueue_samples, 0.99) << " ns\n"
           << "p99.9\t\t\t" << percentile(enqueue_samples, 0.999) / 1.0e6L
           << " ms\n"
           << "p99.99\t\t\t" << percentile(enqueue_samples, 0.9999) / 1.0e6L
           << " ms\n"
           << "p99.999\t\t\t" << percentile(enqueue_samples, 0.99999) / 1.0e6L
           << " ms\n"
           << "max\t\t\t" << enqueue_max_ns / 1.0e6L << " ms\n"
           << "\n"

           << "dequeue latency\n"
           << "-------------\n"
           << "sum\t\t\t" << reduce_as_micros(stats.queue_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t\t" << avg_queue_ns << " ns\n"
           << "min\t\t\t" << stats.queue_min_ns << " ns\n"
           << "p50\t\t\t" << percentile(stats.queue_samples, 0.50) << " ns\n"
           << "p90\t\t\t" << percentile(stats.queue_samples, 0.90) << " ns\n"
           << "p99\t\t\t" << percentile(stats.queue_samples, 0.99) << " ns\n"
           << "p99.9999\t\t"
           << percentile(stats.queue_samples, 0.999999) / 1.0e3L << " µs\n"
           << "max\t\t\t" << stats.queue_max_ns / 1.0e3L << " µs\n"
           << "\n"

           << "memcpy latency\n"
           << "-------------\n"
           << "sum\t\t\t" << reduce_as_micros(stats.memcpy_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t\t" << avg_memcpy_ns << " ns\n"
           << "min\t\t\t" << stats.memcpy_min_ns << " ns\n"
           << "p50\t\t\t" << percentile(stats.memcpy_samples, 0.50) << " ns\n"
           << "p90\t\t\t" << percentile(stats.memcpy_samples, 0.90) << " ns\n"
           << "p99\t\t\t" << percentile(stats.memcpy_samples, 0.99) << " ns\n"
           << "p99.9999\t\t"
           << percentile(stats.memcpy_samples, 0.999999) / 1.0e3L << " µs\n"
           << "max\t\t\t" << stats.memcpy_max_ns / 1.0e3L << " µs\n"
           << "\n"

           << "free latency\n"
           << "-------------\n"
           << "sum\t\t\t" << reduce_as_micros(stats.free_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t\t" << avg_free_ns << " ns\n"
           << "min\t\t\t" << stats.free_min_ns << " ns\n"
           << "p50\t\t\t" << percentile(stats.free_samples, 0.50) << " ns\n"
           << "p90\t\t\t" << percentile(stats.free_samples, 0.90) << " ns\n"
           << "p99\t\t\t" << percentile(stats.free_samples, 0.99) << " ns\n"
           << "p99.9999\t\t"
           << percentile(stats.free_samples, 0.999999) / 1.0e3L << " µs\n"
           << "max\t\t\t" << stats.free_max_ns / 1.0e3L << " µs\n"
           << "\n"

           << "I/O latency (pop)\n"
           << "-----------\n"
           << "sum\t\t" << reduce_as_micros(stats.pop_io_samples) / 1.0e6L
           << " s\n"
           << "avg\t\t" << avg_pop_io_ns / 1.0e3L << " µs\n"
           << "min\t\t" << stats.pop_io_min_ns << " ns\n"
           << "p50\t\t" << percentile(stats.pop_io_samples, 0.50) / 1.0e3L
           << " µs\n"
           << "p90\t\t" << percentile(stats.pop_io_samples, 0.90) / 1.0e3L
           << " µs\n"
           << "p99\t\t" << percentile(stats.pop_io_samples, 0.99) / 1.0e3L
           << " µs\n"
           << "p99.9\t\t" << percentile(stats.pop_io_samples, 0.999) / 1.0e6L
           << " ms\n"
           << "p99.99\t\t" << percentile(stats.pop_io_samples, 0.9999) / 1.0e6L
           << " ms\n"
           << "p99.999\t\t"
           << percentile(stats.pop_io_samples, 0.99999) / 1.0e6L << " ms\n"
           << "max\t\t" << stats.pop_io_max_ns / 1.0e6L << " ms\n"
           << "\n"

           << "I/O latency (push)\n"
           << "-----------\n"
           << "sum\t\t" << reduce_as_micros(stats.push_io_samples) / 1.0e3L
           << " ms\n"
           << "avg\t\t" << avg_push_io_ns << " ns\n"
           << "min\t\t" << stats.push_io_min_ns << " ns\n"
           << "p50\t\t" << percentile(stats.push_io_samples, 0.50) << " ns\n"
           << "p90\t\t" << percentile(stats.push_io_samples, 0.90) << " ns\n"
           << "p99\t\t" << percentile(stats.push_io_samples, 0.99) << " ns\n"
           << "p99.9\t\t" << percentile(stats.push_io_samples, 0.999) / 1.0e3L
           << " µs\n"
           << "p99.99\t\t" << percentile(stats.push_io_samples, 0.9999) / 1.0e3L
           << " µs\n"
           << "p99.999\t\t"
           << percentile(stats.push_io_samples, 0.99999) / 1.0e3L << " µs\n"
           << "max\t\t" << stats.push_io_max_ns / 1.0e3L << " µs\n";

    MESSAGE(report.str());
#elif BENCHMARK == 2
    const double mib_per_second =
        static_cast<double>(byte_count) / elapsed_seconds / (1 << 20);

    std::ostringstream report;
    report << "\n"
           << std::fixed << std::setprecision(2)
           << "MPSC -> dual-buffer WRITEV\n"
           << "==========================\n"
           << "producers\t" << PRODUCERS << "\n"
           << "lines\t\t" << consumed_lines << "\n"
           << "bytes\t\t" << byte_count * (1.0L / (1 << 30)) << " GiB\n"
           << "\n"

           << "throughput\n"
           << "----------\n"
           << "elapsed\t\t" << elapsed_ns / 1.0e9L << " s\n"
           << "lines/sec\t" << lines_per_second << "\n"
           << "MiB/sec\t\t" << mib_per_second << "\n";
    MESSAGE(report.str());
#endif
#endif
}
