#include "shclog/io/mmap.hpp"
#include <algorithm>
#include <atomic>
#include <bits/types/sigset_t.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <linux/io_uring.h>
#include <memory>
#include <span>
#include <sys/syscall.h>
#include <unistd.h>
#include <variant>

namespace shclog::io::iouring {
using namespace shclog::io::mmap;

enum class IoUringSetupError {
    NoSuchFd,
    PermissionDenied,
    ProcessFdQuotaExceeded,
    SystemFdQuotaExceeded,
    OutOfMemory,
    Unexpected,
};

const std::expected<fd_t, IoUringSetupError>
iouring_setup(io_uring_params &params, const uint32_t capacity) noexcept;

enum class IoUringEnterError {
    Unexpected,
};

const std::expected<uint32_t, IoUringEnterError>
iouring_enter(const fd_t ring_fd, const uint32_t to_submit,
              const uint32_t min_complete, const uint32_t flags,
              sigset_t *sig = nullptr) noexcept;

struct EventedIo {
  public:
    enum CreateError {
        InvalidCapacity,
        UnableToSetupRing,
        NoAvailableFd,
        OutOfMemory,
    };

    template <typename T = uint8_t>
    static std::expected<std::unique_ptr<T, MmapDeleter>, CreateError>
    alloc(const fd_t ring_fd, const size_t size, const uint64_t offset) {

        auto mmp_r = mmap::mmap<T>(size, nullptr, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_POPULATE, ring_fd, offset);

        if (!mmp_r)
            switch (mmp_r.error()) {
            case mmap::MmapError::OutOfMemory:
                return std::unexpected(CreateError::OutOfMemory);
            default:
                return std::unexpected(CreateError::UnableToSetupRing);
            }

        return std::move(mmp_r.value());
    }

    static std::expected<std::unique_ptr<EventedIo>, CreateError>
    create(const uint32_t capacity, const uint32_t flags) noexcept {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            return std::unexpected(CreateError::InvalidCapacity);

        io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        params.flags = flags;

        const auto setup_r = iouring_setup(params, capacity);
        if (!setup_r)
            switch (setup_r.error()) {
            case IoUringSetupError::ProcessFdQuotaExceeded:
            case IoUringSetupError::SystemFdQuotaExceeded:
                return std::unexpected(CreateError::NoAvailableFd);
            case IoUringSetupError::OutOfMemory:
                return std::unexpected(CreateError::OutOfMemory);
            default:
                return std::unexpected(CreateError::UnableToSetupRing);
            }

        auto ring_fd = unique_fd(setup_r.value());

        size_t sq_size =
            params.sq_off.array + params.sq_entries * sizeof(uint32_t);
        size_t cq_size =
            params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

        const bool single_mmap = params.features & IORING_FEAT_SINGLE_MMAP;

        if (single_mmap) {
            sq_size = std::max(sq_size, cq_size);
            cq_size = sq_size;
        }

        auto sq_mmap_r = alloc(ring_fd.get(), sq_size, IORING_OFF_SQ_RING);
        if (!sq_mmap_r)
            return std::unexpected(sq_mmap_r.error());

        auto sq_ptr = std::move(sq_mmap_r.value());
        std::unique_ptr<uint8_t, MmapDeleter> cq_ptr = nullptr;

        if (!single_mmap) {
            auto cq_mmap_r = alloc(ring_fd.get(), cq_size, IORING_OFF_CQ_RING);
            if (!cq_mmap_r)
                return std::unexpected(cq_mmap_r.error());

            cq_ptr = std::move(cq_mmap_r.value());
        }

        std::atomic<uint32_t> *const sq_head =
            reinterpret_cast<std::atomic<uint32_t> *>(sq_ptr.get() +
                                                      params.sq_off.head);
        std::atomic<uint32_t> *const sq_tail =
            reinterpret_cast<std::atomic<uint32_t> *>(sq_ptr.get() +
                                                      params.sq_off.tail);

        uint32_t *const sq_flags =
            reinterpret_cast<uint32_t *>(sq_ptr.get() + params.sq_off.flags);
        uint32_t *const sq_array =
            reinterpret_cast<uint32_t *>(sq_ptr.get() + params.sq_off.array);
        uint32_t *const sq_dropped =
            reinterpret_cast<uint32_t *>(sq_ptr.get() + params.sq_off.dropped);
        const uint32_t sq_mask = *reinterpret_cast<uint32_t *>(
            sq_ptr.get() + params.sq_off.ring_mask);

        auto sqes_mmap_r = alloc<io_uring_sqe>(
            ring_fd.get(), params.sq_entries * sizeof(io_uring_sqe),
            IORING_OFF_SQES);
        if (!sqes_mmap_r)
            return std::unexpected(sqes_mmap_r.error());

        auto sqes = std::move(sqes_mmap_r.value());

        uint8_t *const cq_ptr_raw = cq_ptr ? cq_ptr.get() : sq_ptr.get();

        std::atomic<uint32_t> *const cq_head =
            reinterpret_cast<std::atomic<uint32_t> *>(cq_ptr_raw +
                                                      params.cq_off.head);
        std::atomic<uint32_t> *const cq_tail =
            reinterpret_cast<std::atomic<uint32_t> *>(cq_ptr_raw +
                                                      params.cq_off.tail);

        uint32_t *const cq_flags =
            reinterpret_cast<uint32_t *>(sq_ptr.get() + params.cq_off.flags);
        uint32_t *const cq_overflow =
            reinterpret_cast<uint32_t *>(sq_ptr.get() + params.cq_off.overflow);
        const uint32_t cq_mask =
            *reinterpret_cast<uint32_t *>(cq_ptr_raw + params.cq_off.ring_mask);
        io_uring_cqe *const cqes =
            reinterpret_cast<io_uring_cqe *>(cq_ptr_raw + params.cq_off.cqes);

        auto evented = new (std::nothrow)
            EventedIo(std::move(ring_fd), std::move(sq_ptr), std::move(cq_ptr),
                      sq_head, sq_tail, sq_flags, sq_array, sq_dropped, sq_mask,
                      std::move(sqes), cq_head, cq_tail, cq_flags, cq_overflow,
                      cq_mask, cqes, flags & IORING_SETUP_SQPOLL);

        if (!evented)
            return std::unexpected(CreateError::OutOfMemory);

        return std::unique_ptr<EventedIo>(evented);
    }

    enum class PushResult {
        Success,
        WakeFailed,
    };

    PushResult push_writev(const fd_t fd, const std::span<const iovec> iovecs,
                           const size_t offset) noexcept {
        const uint32_t tail = sq_tail->load(std::memory_order_relaxed);
        const uint32_t index = tail & sq_mask;

        auto sqe = &sqes.get()[index];
        sqe->opcode = IORING_OP_WRITEV;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(iovecs.data());
        sqe->len = iovecs.size();
        sqe->off = offset;

        return submit(index, tail);
    }

    enum class PopError {
        RingEmpty,
    };

    enum class WritevError {
        Unexpected,
    };

    using PopWritevError = std::variant<PopError, WritevError>;

    std::expected<const uint32_t, PopWritevError> pop_writev() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value())
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        switch (iouring_errno(rc)) {
        case Errno::SUCCESS:
            break;
        default:
            debug_e_errno(-rc);
            return std::unexpected(WritevError::Unexpected);
        }

        return static_cast<uint32_t>(rc);
    }

    enum class ReadError {
        Unexpected,
    };

    using PopReadError = std::variant<PopError, ReadError>;

    std::expected<const uint32_t, PopReadError> pop_read() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value())
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        switch (iouring_errno(rc)) {
        case Errno::SUCCESS:
            break;
        default:
            debug_e_errno(-rc);
            return std::unexpected(ReadError::Unexpected);
        }

        return static_cast<uint32_t>(rc);
    }

    PushResult push_read(const fd_t fd, const std::span<uint8_t> buff,
                         const size_t offset) noexcept {
        const uint32_t tail = sq_tail->load(std::memory_order_relaxed);
        const uint32_t index = tail & sq_mask;

        auto sqe = &sqes.get()[index];
        sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buff.data());
        sqe->len = buff.size();
        sqe->off = offset;

        return submit(index, tail);
    }

  private:
    unique_fd ring_fd;

    std::unique_ptr<uint8_t, MmapDeleter> sq_ptr;
    std::unique_ptr<uint8_t, MmapDeleter> cq_ptr;

    std::atomic<uint32_t> *const sq_head;
    std::atomic<uint32_t> *const sq_tail;
    uint32_t *const sq_flags;
    uint32_t *const sq_array;
    uint32_t *const sq_dropped;
    const uint32_t sq_mask;
    std::unique_ptr<io_uring_sqe, MmapDeleter> sqes;

    std::atomic<uint32_t> *const cq_head;
    std::atomic<uint32_t> *const cq_tail;
    uint32_t *const cq_flags;
    uint32_t *const cq_overflow;
    const uint32_t cq_mask;
    io_uring_cqe *const cqes;

    const bool is_sq_poll;

    EventedIo(unique_fd ring_fd,

              std::unique_ptr<uint8_t, MmapDeleter> sq_ptr,
              std::unique_ptr<uint8_t, MmapDeleter> cq_ptr,

              std::atomic<uint32_t> *const sq_head,
              std::atomic<uint32_t> *const sq_tail, uint32_t *const sq_flags,
              uint32_t *const sq_array, uint32_t *const sq_dropped,
              const uint32_t sq_mask,
              std::unique_ptr<io_uring_sqe, MmapDeleter> sqes,

              std::atomic<uint32_t> *const cq_head,
              std::atomic<uint32_t> *const cq_tail, uint32_t *const cq_flags,
              uint32_t *const cq_overflow, const uint32_t cq_mask,
              io_uring_cqe *const cqes, const bool is_sq_poll) noexcept
        : ring_fd(std::move(ring_fd)), sq_ptr(std::move(sq_ptr)),
          cq_ptr(std::move(cq_ptr)), sq_head(sq_head), sq_tail(sq_tail),
          sq_flags(sq_flags), sq_array(sq_array), sq_dropped(sq_dropped),
          sq_mask(sq_mask), sqes(std::move(sqes)), cq_head(cq_head),
          cq_tail(cq_tail), cq_flags(cq_flags), cq_overflow(cq_overflow),
          cq_mask(cq_mask), cqes(cqes), is_sq_poll(is_sq_poll) {}

    // params.flags =
    // kernel thread for sq
    //     IORING_SETUP_SQPOLL |
    // single issues (couples with mpsc queue), couples cpu
    //     IORING_SETUP_SINGLE_ISSUER |
    // tight control over 'completion' interruptions, which makes sense for
    // our design
    //     IORING_SETUP_DEFER_TASKRUN;
    // aggressive non-idle iouring thread
    //     params.sq_thread_idle = 0;
    //
    // Same CPU setup (might not be a good idea)
    // params.flags |= IORING_SETUP_SQ_AFF;
    // params.sq_thread_cpu = logger_io_cpu;
    //
    // iopoll might be necessary, lets see the diff later

    std::expected<const int32_t, PopError> pop_one() noexcept {
        auto wait_r = wait_one();
        if (wait_r != WaitResult::Success)
            return std::unexpected(PopError::RingEmpty);

        const uint32_t head = cq_head->load(std::memory_order_acquire);

        if (head == cq_tail->load(std::memory_order_relaxed))
            return std::unexpected(PopError::RingEmpty);

        io_uring_cqe *const cqe = &cqes[head & cq_mask];

        cq_head->store(head + 1, std::memory_order_release);

        return cqe->res;
    }

    enum class WaitResult {
        Success,
        GetFailure,
    };

    WaitResult wait_one() {
        auto r = iouring_enter(ring_fd.get(), 0, 1, IORING_ENTER_GETEVENTS);
        if (!r.has_value())
            return WaitResult::GetFailure;
        return WaitResult::Success;
    }

    PushResult submit(const uint32_t index, const uint32_t tail) noexcept {
        sq_array[index] = index;
        sq_tail->store(tail + 1, std::memory_order_release);
        if (is_sq_poll && (*sq_flags & IORING_SQ_NEED_WAKEUP)) {
            auto r = iouring_enter(ring_fd.get(), 0, 0, IORING_ENTER_SQ_WAKEUP);
            if (!r.has_value())
                return PushResult::WakeFailed;
        } else {
            auto r = iouring_enter(ring_fd.get(), 1, 0, 0);
            if (!r.has_value())
                return PushResult::WakeFailed;
        }

        return PushResult::Success;
    }
};
} // namespace shclog::io::iouring
