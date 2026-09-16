#pragma once

#include "shclog/const.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <emmintrin.h>
#include <expected>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

/* Adapted from https://github.com/dbittman/waitfree-mpsc-queue/tree/master
    Changes:
    - handle out of memory on creation without exploding
    - smart pointers (except for atomic because it doesnt support non-trivially
      copiable types)
    - enforce power of 2 capacity
    - use mask-based wrap around
    - added cacheline split for variables
    - changed atomicity fencing to relaxed where possible
    - drain on free because queue owns data
    - not allowing nullptr inserts
    - moving nullptr insert check + empty dequeue return as unlikely branches
      this causes empty queue checks to be a branch-miss, which is fine because
      congestion behaves better
*/

namespace shclog::mpsc_queue {
enum CreateError : uint8_t { InvalidCapacity, AllocationFailed };

template <typename T, typename Deleter = std::default_delete<T>>
struct alignas(std::hardware_destructive_interference_size) MPSCQueue {
  private:
    static constexpr std::align_val_t buf_align{
        std::hardware_destructive_interference_size};

    struct AlignedBufferDeleter {
        void operator()(std::atomic<T *> *p) const noexcept {
            static_assert(std::is_trivially_destructible_v<std::atomic<T *>>);
            ::operator delete[](p, buf_align);
        }
    };

    using Buffer = std::unique_ptr<std::atomic<T *>[], AlignedBufferDeleter>;

    alignas(std::hardware_destructive_interference_size)
        std::atomic<size_t> count{};
    alignas(
        std::hardware_destructive_interference_size) std::atomic<size_t> head{};
    alignas(std::hardware_destructive_interference_size) Buffer buffer;
    alignas(std::hardware_destructive_interference_size) size_t tail{};
    // padding
    alignas(std::hardware_destructive_interference_size) const size_t buf_mask;
    const size_t max;

    MPSCQueue(const size_t capacity, Buffer &&buf_ptr) noexcept
        : buffer(std::move(buf_ptr)), buf_mask(capacity - 1), max(capacity) {}

  public:
    static std::expected<std::unique_ptr<MPSCQueue<T, Deleter>>, CreateError>
    create(const size_t capacity) noexcept {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::InvalidCapacity);

        auto buf =
            Buffer(new (buf_align, std::nothrow) std::atomic<T *>[capacity]());
        if (!buf) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::AllocationFailed);

        auto queue = std::unique_ptr<MPSCQueue<T, Deleter>>(
            new (std::nothrow) MPSCQueue<T, Deleter>(capacity, std::move(buf)));
        if (!queue) [[unlikely]]
            return std::unexpected<CreateError>(CreateError::AllocationFailed);

        return std::move(queue);
    }

    MPSCQueue(const MPSCQueue &) = delete;
    MPSCQueue operator=(const MPSCQueue &) = delete;

    MPSCQueue(MPSCQueue &&) = default;
    MPSCQueue &operator=(MPSCQueue &&) = default;

    ~MPSCQueue() noexcept {
        while (auto item = dequeue()) {
        }
    };

    bool enqueue(std::unique_ptr<T, Deleter> &&v) noexcept {
        if (!v) [[unlikely]]
            return false;

        // this reduces the chance of backing off all producers
        if (count.load(std::memory_order_relaxed) >= max)
            return false;

        const size_t cur_count = count.fetch_add(1, std::memory_order_acquire);
        if (cur_count >= max) {
            count.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }

        const size_t h = head.fetch_add(1, std::memory_order_relaxed);

        if constexpr (IS_DEBUG) {
            const auto rv = buffer[h & buf_mask].exchange(
                v.release(), std::memory_order_release);
            assert(!rv);
        } else
            buffer[h & buf_mask].store(v.release(), std::memory_order_release);

        return true;
    }

    std::unique_ptr<T, Deleter> dequeue() noexcept {
        const auto ret =
            buffer[tail].exchange(nullptr, std::memory_order_acquire);

        if (!ret)
            return nullptr;

        tail = (tail + 1) & buf_mask;

        if constexpr (IS_DEBUG) {
            const size_t r = count.fetch_sub(1, std::memory_order_release);
            assert(r > 0);
        } else
            count.fetch_sub(1, std::memory_order_release);

        return std::unique_ptr<T, Deleter>(ret);
    }

    [[nodiscard]] size_t size() const noexcept { return count; }

    [[nodiscard]] size_t capacity() const noexcept { return max; }
};

// TODO: remove size from template, add more checks
template <typename T, size_t N, typename Deleter = std::default_delete<T>>
struct MPSCQSlotted {
  private:
    struct alignas(std::hardware_destructive_interference_size) Cell {
        std::atomic<size_t> seq{};
        T *data{nullptr};
    };

  public:
    size_t tail;
    std::atomic<size_t> head;

    Cell buffer[N];

    static constexpr size_t MASK = N - 1;

    MPSCQSlotted() noexcept {
        tail = 0;
        head.store(0, std::memory_order_relaxed);

        for (size_t i = 0; i < N; ++i)
            buffer[i].seq.store(i, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
    }

    bool enqueue(std::unique_ptr<T, Deleter> &&v) noexcept {
        // size_t pos = head.value.load(std::memory_order_relaxed);
        size_t pos = head.fetch_add(1, std::memory_order_relaxed);

        Cell *slot = &buffer[pos & MASK];
        // Cell *slot;
        while (true) {
            // slot = &buffer[pos & MASK];
            const size_t seq = slot->seq.load(std::memory_order_acquire);
            const intptr_t dif =
                static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                // if (head.compare_exchange_weak(pos, pos + 1,
                //                                      std::memory_order_relaxed))
                break;
            }

            // if (dif < 0)
            //     return false;

            _mm_pause();

            // pos = head.load(std::memory_order_relaxed);
        }

        slot->data = v.release();
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    std::unique_ptr<T, Deleter> dequeue() noexcept {
        Cell *slot = &buffer[tail & MASK];

        const size_t seq = slot->seq.load(std::memory_order_acquire);

        const intptr_t dif =
            static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail + 1);
        if (dif < 0)
            return nullptr;

        T *v = slot->data;
        slot->seq.store(tail + N, std::memory_order_release);
        tail++;
        return std::unique_ptr<T, Deleter>(v);
    }
};

struct Node {
    std::atomic<Node *> next{nullptr};
};

template <typename T>
concept HasNodeMember =
    std::is_same_v<decltype(std::declval<T &>().node), Node>;

template <typename T> inline T *container_of(Node *n) noexcept {
    static_assert(std::is_standard_layout_v<T>,
                  "Memory layout has to be C compatible");
    static_assert(HasNodeMember<T>, "T must have a member Node node;");
    auto offset = offsetof(T, node);
    return reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(n) - offset);
}

// TODO: add more checks
template <typename T, typename Deleter = std::default_delete<T>>
struct UnboundedLinkedMPSCQ {
    static_assert(HasNodeMember<T>, "T must have a member Node node;");

  public:
    UnboundedLinkedMPSCQ() noexcept {
        stub.next.store(nullptr, std::memory_order::relaxed);
        head.store(&stub, std::memory_order_relaxed);
        tail = &stub;
        std::atomic_thread_fence(std::memory_order_release);
    }

    ~UnboundedLinkedMPSCQ() {
        while (auto item = dequeue()) {
        }
    }

    UnboundedLinkedMPSCQ(const UnboundedLinkedMPSCQ &) = delete;
    UnboundedLinkedMPSCQ &operator=(const UnboundedLinkedMPSCQ &) = delete;
    UnboundedLinkedMPSCQ(UnboundedLinkedMPSCQ &&) = delete;
    UnboundedLinkedMPSCQ &operator=(UnboundedLinkedMPSCQ &&) = delete;

    void enqueue(std::unique_ptr<T, Deleter> item) noexcept {
        T *raw = item.release();
        push(&raw->node);
    }

    std::unique_ptr<T, Deleter> dequeue() noexcept {
        Node *node = pop();
        if (!node)
            return nullptr;
        return std::unique_ptr<T, Deleter>(container_of<T>(node));
    }

  private:
    std::atomic<Node *> head;
    Node *tail;
    Node stub;

    void push(Node *n) noexcept {
        n->next = nullptr;
        auto prev = head.exchange(n, std::memory_order_acq_rel);
        prev->next.store(n, std::memory_order_release);
    }

    Node *pop() noexcept {
        Node *local_tail = tail;
        Node *next = local_tail->next.load(std::memory_order_acquire);

        if (local_tail == &stub) {
            if (!next)
                return nullptr;

            local_tail = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }

        if (next) {
            tail = next;
            return local_tail;
        }

        Node *local_head = head.load(std::memory_order_acquire);
        if (local_tail != local_head) {
            // Producer exchanged head but not linked prev->next yet
            return nullptr;
        }

        // tail == head, no next, they arent stubs, move next to stub
        // or something (on race) and re-check
        push(&stub);

        next = local_tail->next.load(std::memory_order_acquire);
        if (next) {
            tail = next;
            return local_tail;
        }

        // retriable sync between new push prev->next moving (prev being tail)
        return nullptr;
    }
};
} // namespace shclog::mpsc_queue
