#pragma once

#include "shclog/align.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <memory>
#include <new>

/* Adapted from https://github.com/dbittman/waitfree-mpsc-queue/tree/master
    Changes:
    - handle out of memory on creation without exploding
    - smart pointers (except for atomic because it doesnt support non-trivially
      copiable types)
    - enforce power of 2 capacity
    - use mask-based wrap around
    - added cacheline split for variables
    - changed atomicity fencing to relaxed
        - enqueue doesnt need acquire/release because the value itself
          changed is atomic, which is the case for buffer, head and count
        - dequeue doesnt need it because tail is supposed to be used single
          threaded, count and buffer are changed in atomics
    - drain on free because queue owns data
    - not allowing nullptr inserts
    - moving nullptr insert check + empty dequeue return as unlikely branches
      this causes empty queue checks to be a branch-miss, which is fine because
      congestion behaves better
*/

namespace shclog::mpsc_queue {
enum CreateError { InvalidCapacity, AllocationFailed };

template <typename T>
struct alignas(std::hardware_destructive_interference_size) MPSCQueue {
  private:
    align::CachePadAlign<std::atomic<size_t>> count;
    align::CachePadAlign<std::atomic<size_t>> head;
    align::CachePadAlign<std::unique_ptr<std::atomic<T *>[]>> buffer;
    align::CachePadAlign<size_t> tail;
    const size_t buf_mask;
    const size_t max;

    MPSCQueue(const size_t capacity,
              std::unique_ptr<std::atomic<T *>[]> &&buf_ptr) noexcept
        : count(), head(), buffer(std::move(buf_ptr)), tail(),
          buf_mask(capacity - 1), max(capacity) {}

  public:
    static std::expected<std::unique_ptr<MPSCQueue<T>>, CreateError>
    create(const size_t capacity) noexcept {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::InvalidCapacity);

        // done like this for alignment
        alignas(std::hardware_destructive_interference_size)
            std::atomic<T *> *const raw_buf =
                new (std::nothrow) std::atomic<T *>[capacity]();
        auto buf = std::unique_ptr<std::atomic<T *>[]>(raw_buf);

        if (!buf) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::AllocationFailed);

        auto queue = std::unique_ptr<MPSCQueue<T>>(
            new (std::nothrow) MPSCQueue<T>(capacity, std::move(buf)));
        if (!queue) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::AllocationFailed);

        return std::move(queue);
    }

    MPSCQueue(const MPSCQueue &) = delete;
    MPSCQueue operator=(const MPSCQueue &) = delete;

    MPSCQueue(MPSCQueue &&) = default;
    MPSCQueue &operator=(MPSCQueue &&) = default;

    ~MPSCQueue() noexcept {
        for (auto item = dequeue(); item; item = dequeue()) {
        }
    };

    bool enqueue(std::unique_ptr<T> &&v) noexcept {
        if (!v) [[unlikely]]
            return false;

        const size_t cur_count =
            count.value.fetch_add(1, std::memory_order_relaxed);
        if (cur_count >= max) {
            count.value.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }

        const size_t h = head.value.fetch_add(1, std::memory_order_relaxed);

        assert(buffer.value[h & buf_mask].load(std::memory_order_relaxed) ==
               nullptr);
        const auto rv = buffer.value[h & buf_mask].exchange(
            v.release(), std::memory_order_relaxed);
        assert(!rv);

        return true;
    }

    std::unique_ptr<T> dequeue() noexcept {
        const auto ret = buffer.value[tail.value].exchange(
            nullptr, std::memory_order_relaxed);

        if (!ret)
            return nullptr;

        tail.value = (tail.value + 1) & buf_mask;

        const size_t r = count.value.fetch_sub(1, std::memory_order_relaxed);
        assert(r > 0);
        return std::unique_ptr<T>(ret);
    }

    size_t size() const noexcept { return count.value; }

    size_t capacity() const noexcept { return max; }
};
} // namespace shclog::mpsc_queue
