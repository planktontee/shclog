#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "doctest.h"
#include "shclog/mpsc_queue.hpp"
#include "shclog/pause.hpp"
#include <algorithm>
#include <cstddef>
#include <emmintrin.h>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <thread>

using namespace shclog::mpsc_queue;
using namespace shclog::pause;

TEST_CASE("MPSCQueue basic operations") {
    constexpr size_t capacity = 8;
    auto result = MPSCQueue<int>::create(capacity);

    CHECK(result.has_value());

    const auto queue = std::move(result.value());
    CHECK(queue->capacity() == capacity);
    CHECK(queue->size() == 0);

    REQUIRE(queue->dequeue() == nullptr);

    CHECK(queue->enqueue(std::make_unique<int>(42)) == true);
    CHECK(queue->size() == 1);

    for (auto item = queue->dequeue(); item; item = queue->dequeue()) {
        CHECK(*item == 42);
    }

    CHECK(queue->size() == 0);
}

TEST_CASE("MPSCQueue capacity limit") {
    constexpr size_t capacity = 4;
    auto result = MPSCQueue<int>::create(capacity);
    CHECK(result.has_value());

    const auto queue = std::move(result.value());

    for (size_t i = 0; i < capacity; ++i) {
        CHECK(queue->enqueue(std::make_unique<int>(static_cast<int>(i))));
    }

    CHECK(queue->enqueue(std::make_unique<int>(999)) == false);

    size_t count = 0;
    for (auto item = queue->dequeue(); item; item = queue->dequeue()) {
        count++;
    }

    CHECK(count == capacity);
    CHECK(queue->size() == 0);
}

#define RUN_CHECKS 1

TEST_CASE("MPSCQueue concurrent test") {
#if RUN_CHECKS
    constexpr size_t capacity = (1 << 10);
    constexpr int n_producers = 3;
    constexpr int target_items = (1 << 10) * 16;
#else
    constexpr size_t capacity = (1 << 10) * 32;
    constexpr int n_producers = 15;
    constexpr int target_items = (1 << 20) * 16;
#endif

    auto r = MPSCQueue<int>::create(capacity);
#if RUN_CHECKS
    CHECK(r.has_value());
#endif
    const std::unique_ptr<MPSCQueue<int>> queue = std::move(r.value());

    std::atomic<bool> producers_done = false;

    std::vector<std::jthread> producers;
    producers.reserve(n_producers);

#if RUN_CHECKS
    constexpr int total_items = target_items * n_producers;
    auto check_ptr = std::make_unique<bool[]>(total_items);
    std::span<bool> seen_items(check_ptr.get(), total_items);

    std::atomic<int> total_enqueued = 0;
    std::atomic<int> total_dequeued = 0;
#endif

    for (int p = 0; p < n_producers; ++p) {
#if RUN_CHECKS
        producers.emplace_back([&queue, &total_enqueued, p]() {
            int local_enqueued = 0;
#else
        producers.emplace_back([&queue, p]() {
#endif
            auto items = std::unique_ptr<std::unique_ptr<int>[]>(
                new (std::nothrow) std::unique_ptr<int>[target_items]);
            for (int i = 0; i < target_items; ++i) {
                items[i] = std::make_unique<int>(p * target_items + i + 1);
            }

            for (int i = 0; i < target_items; ++i) {
                while (!queue->enqueue(std::move(items[i]))) {
                    std::this_thread::yield();
                }
#if RUN_CHECKS
                local_enqueued++;
#endif
            }
#if RUN_CHECKS
            total_enqueued.fetch_add(local_enqueued, std::memory_order_relaxed);
#endif
        });
    }

#
#if RUN_CHECKS
    std::jthread consumer(
        [&queue, &producers_done, &seen_items, &total_dequeued]() {
            int target = n_producers * target_items;
            int local_consumed = 0;

            while (local_consumed < target) {
#else

    std::jthread consumer([&queue, &producers_done]() {
        while (true) {
#endif
                auto item = queue->dequeue();

                if (item) {
#if RUN_CHECKS
                    int value = *item;

                    REQUIRE(value > 0);
                    REQUIRE(value < total_items + 1);

                    seen_items[value - 1] = true;
                    local_consumed++;
#endif
                } else {
                    if (producers_done.load(std::memory_order_acquire)) {
                        auto item = queue->dequeue();
                        if (!item)
                            break;
                        else {
#if RUN_CHECKS
                            int value = *item;

                            REQUIRE(value > 0);
                            REQUIRE(value < total_items + 1);

                            seen_items[value - 1] = true;
                            local_consumed++;
#endif
                            continue;
                        }
                    } else
                        continue;
                }
            }
#if RUN_CHECKS
            total_dequeued.fetch_add(local_consumed, std::memory_order_relaxed);
#endif
        });

    producers.clear();
    producers_done.store(true, std::memory_order_release);
    consumer.join();

#if RUN_CHECKS
    CHECK(total_enqueued.load() == total_items);
    CHECK(total_dequeued.load() == total_items);
    CHECK(queue->size() == 0);
    CHECK(std::all_of(seen_items.begin(), seen_items.end(), std::identity()));
#endif
}
