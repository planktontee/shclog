#include "shclog/doctest.hpp"

#include "shclog/bench/report.hpp"
#include "shclog/bench/sample.hpp"
#include "shclog/bench/time.hpp"
#include "shclog/cast.hpp"
#include "shclog/io/cpu.hpp"
#include "shclog/io/iouring.hpp"
#include "shclog/io/process.hpp"
#include "shclog/io/syscall.hpp"
#include "shclog/lang.hpp"
#include "shclog/mpsc_queue.hpp"
#include "shclog/types.hpp"
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
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace shclog;
using namespace shclog::lang;
using namespace shclog::bench::report;
using namespace shclog::bench::sample;
using namespace shclog::bench::time;
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
    u8 r_buf_1[12]{};
    const iovec buffers[2] = {
        {.iov_base = w_buf_1, .iov_len = 16},
        {.iov_base = r_buf_1, .iov_len = 12},
    };

    const iovec iovecs[2] = {
        {.iov_base = w_buf_1, .iov_len = 6},
        {.iov_base = w_buf_1 + 6, .iov_len = 10},
    };
    CHECK(evented->register_buffers(std::span{buffers, 2}));

    auto writev_push_r = evented->push_writev(
        tmp_fd_idx,
        std::span{iovecs, 2},
        0,
        IOSQE_FIXED_FILE,
        true,
        0
    );
    CHECK(writev_push_r == EventedIo::PushResult::Success);

    auto writev_pop_r = evented->pop_writev();
    REQUIRE(writev_pop_r.has_value());
    CHECK(writev_pop_r.value() == 16);

    auto read_push_r = evented->push_read(
        tmp_fd_idx,
        std::span{r_buf_1, 12},
        0,
        IOSQE_FIXED_FILE,
        true,
        1
    );
    CHECK(read_push_r == EventedIo::PushResult::Success);

    auto read_pop_r = evented->pop_read();
    REQUIRE(read_pop_r.has_value());
    CHECK(read_pop_r.value() == 12);
    CHECK(
        std::string_view(
            reinterpret_cast<const char *>(r_buf_1),
            read_pop_r.value()
        ) == "hello world!"
    );
}

#define RUN_CHECKS 1
#define BENCHMARK 0
#define QUEUE_TYPE 2

#if BENCHMARK
template <class R, shclog::math::Arithmetic T>
std::string unit_repr(const T v, const typename R::Unit u) {
    std::string out;
    out.resize_and_overwrite(
        R::template REPR_MAX_LEN<T>,
        [&](char *const p, const usize cap) {
            Slice<u8> buf(ptr_cast<u8>(p), cap);
            return unwrap(R::write(v, u, buf));
        }
    );
    return out;
}

template <shclog::math::Arithmetic T> std::string fmt_ns(const T v) {
    return unit_repr<TimeUnit>(v, TimeUnit::Unit::nanoseconds);
}

template <shclog::math::Arithmetic T> std::string fmt_bytes(const T v) {
    return unit_repr<ByteUnit>(v, ByteUnit::Unit::bytes);
}

struct BenchReport {
    static constexpr int LABEL_WIDTH = 12;
    static constexpr std::pair<std::string_view, f64> PERCENTILES[] = {
        {"p50", 0.50},
        {"p90", 0.90},
        {"p99", 0.99},
        {"p99.9", 0.999},
        {"p99.99", 0.9999},
        {"p99.999", 0.99999},
        {"p99.9999", 0.999999},
    };

    std::ostringstream out;

    BenchReport() { out << std::fixed << std::setprecision(2); }

    void title(const std::string_view name) {
        out << '\n' << name << '\n' << std::string(name.size(), '=') << '\n';
    }

    void section(const std::string_view name) {
        out << '\n' << name << '\n' << std::string(name.size(), '-') << '\n';
    }

    void row(const std::string_view label, const auto &value) {
        out << std::left << std::setw(LABEL_WIDTH) << label << value << '\n';
    }

    void summary(
        const usize producers,
        const usize lines,
        const u64 bytes,
        const u64 elapsed_ns
    ) {
        const f64 seconds = float_cast<f64>(elapsed_ns) / 1.0e9;

        title("MPSC -> dual-buffer WRITEV");
        row("producers", producers);
        row("lines", lines);
        row("bytes", fmt_bytes(bytes));

        section("throughput");
        row("elapsed", fmt_ns(elapsed_ns));
        row("lines/sec", float_cast<f64>(lines) / seconds);
        row("bytes/sec", fmt_bytes(float_cast<f64>(bytes) / seconds));
    }

    void latency(const std::string_view name, Sample<u64> &s) {
        REQUIRE(!s.empty());
        const auto total = unwrap(s.total());
        REQUIRE(!total.overflow);

        section(name);
        row("sum", fmt_ns(total.value));
        row("avg", fmt_ns(unwrap(s.avg())));
        row("min", fmt_ns(unwrap(s.min())));
        for (const auto &[label, p] : PERCENTILES)
            row(label, fmt_ns(unwrap(s.percentile(p))));
        row("max", fmt_ns(unwrap(s.max())));
    }
};
#endif

TEST_CASE("MPSC -> dual-buffer/memcpy WRITE") {
    using clock = std::chrono::steady_clock;

#if BENCHMARK
    auto cpu_cores_r = list_cpu_cores();
    CHECK(cpu_cores_r.has_value());
    const auto &cores = cpu_cores_r.value();

    const std::set<usize> target_cores = [&] {
        std::set<usize> shadow;
        for (const auto &core : cores)
            shadow.insert(core.siblings[0]);
        if (shadow.size() == 0)
            shadow.insert(0);
        return shadow;
    }();

    // MAX - main - iouring
    const usize PRODUCERS =
        std::max(usize{1}, std::sub_sat(target_cores.size(), usize{2}));
    // this should be a ratio for MAX_HW_CORES
    constexpr usize LINES_PER_PRODUCER = (1L << 10) * 512;
#if QUEUE_TYPE != 2
    constexpr usize QUEUE_CAPACITY = (1L << 10) * 32;
#endif
    constexpr usize MAX_IO_BYTES = (1L << 10) * 128;
    constexpr usize MIN_LEN = 128;
    constexpr usize MAX_LINE_LEN = (1L << 10) * 2;
#else
    constexpr usize PRODUCERS = 3;
    constexpr usize LINES_PER_PRODUCER = (1L << 5);
#if QUEUE_TYPE != 2
    constexpr usize QUEUE_CAPACITY = 512;
#endif
    constexpr usize MAX_IO_BYTES = (1L << 10);
    constexpr usize MIN_LEN = 64;
    constexpr usize MAX_LINE_LEN = 256;
#endif
    const usize TOTAL_LINES = PRODUCERS * LINES_PER_PRODUCER;
    constexpr usize IO_BUFFERS = 2;
    constexpr auto TARGET_FLUSH =
        std::chrono::nanoseconds(static_cast<u64>(1e9 / 30));
    constexpr usize PREALLOC_CHUNK = MAX_IO_BYTES << 8;
    constexpr usize ALLOC_WATERMARK = MAX_IO_BYTES << 1;

    struct Message {
#if QUEUE_TYPE == 2
        Node node;
#endif
        u8 *data;
        const usize size;

        explicit Message(const usize size) noexcept
            : data(ptr_cast<u8>(this + 1))
            , size(size) {}

        std::span<u8> bytes() noexcept { return {data, size}; }

        [[nodiscard]] std::span<const u8> bytes() const noexcept {
            return {data, size};
        }

        static void *operator new(
            const usize header,
            const usize payload,
            const std::nothrow_t &
        ) noexcept {
            return ::operator new(header + payload, std::nothrow);
        }

        static void operator delete(void *const p) noexcept {
            ::operator delete(p);
        }

        static void
        operator delete(void *const p, usize, const std::nothrow_t &) noexcept {
            ::operator delete(p);
        }
    };

#if BENCHMARK == 1
    struct Stats {
        Sample<u64> falloc;
        Sample<u64> queue;
        Sample<u64> memcpy;
        Sample<u64> free;
        Sample<u64> push;
        Sample<u64> pop;

        u64 bytes = 0;

        u64 writes = 0;
        u64 total_batch_lines = 0;
        u64 max_batch_lines = 0;

        std::vector<Sample<u64>> producers;

        Stats(
            const usize consumer_capacity,
            const usize producers_n,
            const usize producer_capacity,
            const usize max_line_len
        ) noexcept
            : falloc(unwrap(
                  Sample<u64>::make(
                      max_line_len * consumer_capacity / ALLOC_WATERMARK
                  )
              ))
            , queue(unwrap(Sample<u64>::make(consumer_capacity)))
            , memcpy(unwrap(Sample<u64>::make(consumer_capacity)))
            , free(unwrap(Sample<u64>::make(consumer_capacity)))
            , push(unwrap(Sample<u64>::make(consumer_capacity)))
            , pop(unwrap(Sample<u64>::make(consumer_capacity))) {

            producers.reserve(producers_n);
            for (usize i = 0; i < producers_n; ++i)
                producers.push_back(
                    unwrap(Sample<u64>::make(producer_capacity))
                );
        }
    };

    Stats stats(TOTAL_LINES, PRODUCERS, LINES_PER_PRODUCER, MAX_LINE_LEN);
    Time c_time{};
#elif BENCHMARK == 2
    u64 byte_count = 0;
#endif

    const auto make_line =
        [](std::mt19937_64 &rng) -> std::unique_ptr<Message> {
        std::uniform_int_distribution<usize> length_dist(MIN_LEN, MAX_LINE_LEN);

        std::uniform_int_distribution<i32> character_dist(32, 126);

        const usize len = length_dist(rng);
        auto message =
            std::unique_ptr<Message>(new (len, std::nothrow) Message(len));
        if (!message) [[unlikely]]
            std::abort();

        for (usize i = 0; i + 1 < len; ++i)
            message->data[i] = int_cast(character_dist(rng));
        message->data[len - 1] = '\n';

        return message;
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
    MPSCQSlotted<Message, QUEUE_CAPACITY> queue{};
#elif QUEUE_TYPE == 2
    UnboundedLinkedMPSCQ<Message> queue{};
#else
    auto queue_r = MPSCQueue<Message>::create(QUEUE_CAPACITY);
#if RUN_CHECKS
    REQUIRE(queue_r.has_value());
#endif
    auto queue = std::move(queue_r.value());
#endif

#if RUN_CHECKS
    std::vector<u8> expected;
    expected.reserve(TOTAL_LINES * MAX_LINE_LEN);
#endif

    std::atomic<usize> producers_finished = 0;
    std::vector<std::jthread> producers;
    producers.reserve(PRODUCERS);
#if BENCHMARK
    // CHECK(SetPriorityResult::Success == set_priority(-20));
    auto cpu_view =
        std::views::iota(*target_cores.begin(), *target_cores.rbegin() + 1) |
        std::views::filter([&target_cores](usize x) {
            return target_cores.contains(x);
        });
    CHECK(SetCpuAffinityResult::Success == set_cpu_afinity(cpu_view));

    REQUIRE(target_cores.size() >= 1);
    const std::vector<usize> core_arr(target_cores.begin(), target_cores.end());

    const usize main_cpu = int_cast<usize>(sched_getcpu());
    const auto pick_cpu = [main_cpu, &core_arr](const usize target) -> usize {
        const auto idx = target % core_arr.size();
        if (core_arr[idx] >= main_cpu)
            return core_arr[(idx + 1) % core_arr.size()];
        else
            return core_arr[idx];
    };
#endif

    const usize barrier_n = PRODUCERS + 1;
    REQUIRE(barrier_n <= std::barrier<>::max());
    std::barrier ready(int_cast<isize>(barrier_n));
    for (usize producer_id = 0; producer_id < PRODUCERS; ++producer_id) {
        producers.emplace_back([&, producer_id] {
#if BENCHMARK
            CHECK(
                SetCpuAffinityResult::Success ==
                set_cpu_afinity(pick_cpu(producer_id))
            );
            // CHECK(SetPriorityResult::Success == set_priority(-20));
            Time p_time;
#endif
            ready.arrive_and_wait();
            std::mt19937_64 rng(0x8f3a21c7d94e6b5ULL + producer_id);

            for (usize line = 0; line < LINES_PER_PRODUCER; ++line) {
                auto message = make_line(rng);
#if BENCHMARK == 1
                p_time.start();
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
                unwrap(p_time.sample(stats.producers[producer_id]));
#endif
            }

            producers_finished.fetch_add(1, std::memory_order_release);
        });
    }

    alignas(4096) std::array<std::array<u8, MAX_IO_BYTES>, IO_BUFFERS>
        buffers{};

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

    std::array<usize, IO_BUFFERS> line_count{};
    std::array<u64, IO_BUFFERS> io_bytes{};
    std::array<bool, IO_BUFFERS> in_flight{};

    u64 file_offset = 0;
    usize active_buffer = 0;
    usize consumed_lines = 0;
    std::unique_ptr<Message> overflow = nullptr;
    usize last_expand = 0;

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
            c_time.start();
#endif
            assert(std::in_range<off_t>(last_expand));
            [[maybe_unused]] auto rc = fallocate(
                tmp_fd.get(),
                FALLOC_FL_KEEP_SIZE,
                int_cast<off_t>(last_expand),
                PREALLOC_CHUNK
            );
#if BENCHMARK == 1
            unwrap(c_time.sample(stats.falloc));
#endif
#if RUN_CHECKS == 1
            CHECK(rc == 0);
#endif
            last_expand += PREALLOC_CHUNK;
        }

        while ((clock::now() - collection_start) < TARGET_FLUSH &&
               io_bytes[active_buffer] < MAX_IO_BYTES &&
               consumed_lines < TOTAL_LINES) {

            std::unique_ptr<Message> message;
            if (!overflow.get()) [[likely]] {
#if BENCHMARK == 1
                c_time.start();
#endif
#if QUEUE_TYPE == 1
                message = queue.dequeue();
#elif QUEUE_TYPE == 2
                message = queue.dequeue();
#else
                message = queue->dequeue();
#endif
            } else
                message = std::move(overflow);

            if (!message) [[unlikely]] {
                if (producers_finished.load(std::memory_order_acquire) ==
                    PRODUCERS)
                    break;

                continue;
            }
#if BENCHMARK == 1
            if (c_time.started()) [[likely]]
                unwrap(c_time.sample(stats.queue));
#endif

            const auto data_span = message.get()->bytes();

            if (io_bytes[active_buffer] + data_span.size() >=
                buffers[active_buffer].size()) [[unlikely]] {
                overflow = std::move(message);
                break;
            } else {
#if RUN_CHECKS
                expected
                    .insert(expected.end(), data_span.begin(), data_span.end());
#endif
                io_bytes[active_buffer] += data_span.size();
#if BENCHMARK == 1
                c_time.start();
#endif
                std::memcpy(buffer_iter, data_span.data(), data_span.size());
#if BENCHMARK == 1
                unwrap(c_time.sample(stats.memcpy));
#elif BENCHMARK == 2
                byte_count += data_span.size();
#endif
                buffer_iter += data_span.size();
                ++line_count[active_buffer];
                ++consumed_lines;

#if BENCHMARK == 1
                c_time.start();
#endif
                message.reset();
#if BENCHMARK == 1
                unwrap(c_time.sample(stats.free));
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
            c_time.start();
#endif
            [[maybe_unused]] const auto pop_r = evented->pop_write();
#if BENCHMARK == 1
            unwrap(c_time.sample(stats.pop));
#endif

#if RUN_CHECKS
            CHECK(
                pop_r.value() == static_cast<i32>(io_bytes[in_flight_buffer])
            );
#endif
            in_flight[in_flight_buffer] = false;
        }

#if BENCHMARK == 1
        c_time.start();
#endif
#if RUN_CHECKS
        const auto push_r =
#endif
            evented->push_write(
                tmp_fd_idx,
                std::span<const u8>{
                    buffers[active_buffer].data(),
                    io_bytes[active_buffer]
                },
                file_offset,
                IOSQE_FIXED_FILE,
                0,
                true,
                int_cast(active_buffer)
            );
#if BENCHMARK == 1
        unwrap(c_time.sample(stats.push));
#endif
#if RUN_CHECKS
        CHECK(push_r == EventedIo::PushResult::Success);
#endif

#if BENCHMARK == 1
        ++stats.writes;
        stats.max_batch_lines = std::max(
            stats.max_batch_lines,
            static_cast<u64>(line_count[active_buffer])
        );
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
    for (usize buffer = 0; buffer < IO_BUFFERS; ++buffer) {
        if (!in_flight[buffer])
            continue;

#if BENCHMARK == 1
        c_time.start();
#endif

        [[maybe_unused]] const auto pop_r = evented->pop_write();

#if BENCHMARK == 1
        unwrap(c_time.sample(stats.pop));
#endif

#if RUN_CHECKS
        REQUIRE(pop_r.has_value());
        CHECK(pop_r.value() == static_cast<i32>(io_bytes[buffer]));
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
    std::vector<u8> actual(expected.size());
    usize read_offset = 0;
    while (read_offset < actual.size()) {
        const usize remaining = actual.size() - read_offset;

        const usize chunk = std::min(remaining, MAX_IO_BYTES);
        auto push_r = evented->push_read(
            tmp_fd_idx,
            std::span{actual.data() + read_offset, chunk},
            int_cast(read_offset),
            IOSQE_FIXED_FILE
        );

        CHECK(push_r == EventedIo::PushResult::Success);

        const auto pop_r = evented->pop_read();

        REQUIRE(pop_r.has_value());
        CHECK(pop_r.value() == static_cast<i32>(chunk));

        read_offset += chunk;
    }

    CHECK(read_offset == expected.size());

    usize mismatch = expected.size();
    for (usize i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            mismatch = i;
            break;
        }
    }

    if (mismatch != expected.size()) {
        MESSAGE(
            "first mismatch at byte "
            << mismatch
            << " expected=" << static_cast<unsigned>(expected[mismatch])
            << " actual=" << static_cast<unsigned>(actual[mismatch])
        );

        CHECK(mismatch == expected.size());
    }
#endif

#if BENCHMARK
    const auto elapsed_ns =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             benchmark_completed - benchmark_start
        )
                             .count());

    BenchReport report;

#if BENCHMARK == 1
    report.summary(PRODUCERS, consumed_lines, stats.bytes, elapsed_ns);

    report.section("batching");
    report.row("writes", stats.writes);
    report.row(
        "avg lines",
        float_cast<f64>(consumed_lines) / float_cast<f64>(stats.writes)
    );
    report.row("max lines", stats.max_batch_lines);
    report.row(
        "avg bytes",
        fmt_bytes(float_cast<f64>(stats.bytes) / float_cast<f64>(stats.writes))
    );

    Sample<u64> all_producers = unwrap(
        Sample<u64>::make(
            std::transform_reduce(
                stats.producers.begin(),
                stats.producers.end(),
                usize{0},
                std::plus<>{},
                [](const auto &s) { return s.count; }
            )
        )
    );
    for (const auto &samples : stats.producers)
        std::ranges::for_each(samples.span(), [&all_producers](const u64 v) {
            unwrap(all_producers.push(v));
        });

    report.latency("falloc latency", stats.falloc);
    report.latency("enqueue latency", all_producers);
    report.latency("dequeue latency", stats.queue);
    report.latency("memcpy latency", stats.memcpy);
    report.latency("free latency", stats.free);
    report.latency("I/O latency (pop)", stats.pop);
    report.latency("I/O latency (push)", stats.push);
#elif BENCHMARK == 2
    report.summary(PRODUCERS, consumed_lines, byte_count, elapsed_ns);
#endif

    MESSAGE(report.out.str());
#endif
}
