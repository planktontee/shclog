#pragma once

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
#include <optional>
#include <span>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
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

const std::expected<const fd_t, IoUringSetupError>
io_uring_setup(io_uring_params &params, const uint32_t capacity) noexcept;

enum class IoUringEnterError {
    ResourcesTemporarilyUnavailable,
    BadFdForRing,
    RingIsDisabled,
    CompletionQueueIsFull,
    SubmissionQueueIsFull,
    Unexpected,
};

const std::expected<const uint32_t, IoUringEnterError>
io_uring_enter(const fd_t ring_fd, const uint32_t to_submit,
               const uint32_t min_complete, const uint32_t flags,
               sigset_t *sig = nullptr) noexcept;

enum class IoUringRegisterError {
    AccessDenied,
    RegistrationsAlreadyInPlace,
    BadFd,
    BadBuffers,
    BadRequest,
    TooManyFiles,
    OutOfMem,
    RegistrationFailed,
    Unexpected,
};

const std::expected<const uint32_t, IoUringRegisterError>
io_uring_register(const fd_t ring_fd, const uint32_t opcode, const void *arg,
                  uint32_t nr_args) noexcept;

enum class RingQueueAllocError {
    OutOfMemory,
    Unexpected,
};

template <typename T = uint8_t>
static std::expected<std::unique_ptr<T, MmapDeleter>, RingQueueAllocError>
io_uring_mmap(const fd_t ring_fd, const size_t size, const uint64_t offset) {

    auto mmp_r = mmap::mmap<T>(size, nullptr, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, ring_fd, offset);

    if (!mmp_r)
        switch (mmp_r.error()) {
        case mmap::MmapError::OutOfMemory:
            return std::unexpected(RingQueueAllocError::OutOfMemory);
        default:
            debug_e_errno();
            return std::unexpected(RingQueueAllocError::Unexpected);
        }

    return std::move(mmp_r.value());
}

// This is not thread safe, for it to be thread safe
// we would need changes in the release/acquire model and add another stamping
// to the push and pull process
struct EventedIo {
  public:
    enum CreateError {
        InvalidCapacity,
        UnableToSetupRing,
        NoAvailableFd,
        OutOfMemory,
    };

    ~EventedIo() noexcept {
        assert(io_uring_register(ring_fd.get(), IORING_UNREGISTER_BUFFERS,
                                 nullptr, 0)
                   .has_value());
        assert(io_uring_register(ring_fd.get(), IORING_UNREGISTER_FILES,
                                 nullptr, 0)
                   .has_value());

        unregister_ring_fd();
    }

    static std::expected<std::unique_ptr<EventedIo>, CreateError>
    create(const uint32_t capacity, const uint32_t flags) noexcept {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            return std::unexpected(CreateError::InvalidCapacity);

        io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        params.flags = flags;

        const auto setup_r = io_uring_setup(params, capacity);
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

        auto sq_mmap_r =
            io_uring_mmap(ring_fd.get(), sq_size, IORING_OFF_SQ_RING);
        if (!sq_mmap_r)
            switch (sq_mmap_r.error()) {
            case RingQueueAllocError::OutOfMemory:
                return std::unexpected(CreateError::OutOfMemory);
            default:
                return std::unexpected(CreateError::UnableToSetupRing);
            }

        auto sq_ptr = std::move(sq_mmap_r.value());
        std::unique_ptr<uint8_t, MmapDeleter> cq_ptr = nullptr;

        if (!single_mmap) {
            auto cq_mmap_r =
                io_uring_mmap(ring_fd.get(), cq_size, IORING_OFF_CQ_RING);
            if (!cq_mmap_r)
                switch (cq_mmap_r.error()) {
                case RingQueueAllocError::OutOfMemory:
                    return std::unexpected(CreateError::OutOfMemory);
                default:
                    return std::unexpected(CreateError::UnableToSetupRing);
                }

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

        auto sqes_mmap_r = io_uring_mmap<io_uring_sqe>(
            ring_fd.get(), params.sq_entries * sizeof(io_uring_sqe),
            IORING_OFF_SQES);
        if (!sqes_mmap_r)
            switch (sqes_mmap_r.error()) {
            case RingQueueAllocError::OutOfMemory:
                return std::unexpected(CreateError::OutOfMemory);
            default:
                return std::unexpected(CreateError::UnableToSetupRing);
            }

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

        evented->register_ring_fd();

        return std::unique_ptr<EventedIo>(evented);
    }

    bool register_files(const std::span<const fd_t> files) noexcept {
        const auto reg_r = io_uring_register(
            ring_fd.get(), IORING_REGISTER_FILES,
            reinterpret_cast<const void *>(files.data()), files.size());
        if (!reg_r.has_value())
            return false;
        return true;
    }

    bool register_buffers(const std::span<const iovec> bufs) noexcept {
        const auto reg_r = io_uring_register(
            ring_fd.get(), IORING_REGISTER_BUFFERS,
            reinterpret_cast<const void *>(bufs.data()), bufs.size());
        if (!reg_r.has_value())
            return false;
        return true;
    }

    enum class PushResult {
        Success,
        WakeFailed,
        QueueIsFull,
    };

    PushResult push_write(const fd_t fd, const std::span<const uint8_t> buf,
                          const size_t offset, const uint8_t flags = 0,
                          const uint32_t rw_flags = 0,
                          const bool fixed_buffers = false,
                          const uint32_t buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot) [[unlikely]]
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const uint32_t index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_WRITE_FIXED;
        else
            sqe->opcode = IORING_OP_WRITE;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buf.data());
        sqe->len = buf.size();
        sqe->off = offset;
        sqe->flags = flags;
        if (fixed_buffers) {
            sqe->buf_index = buf_idx;
        }
        sqe->rw_flags = rw_flags;

        return submit(slot, index);
    }

    // TODO: add rw_flags
    PushResult push_writev(const fd_t fd, const std::span<const iovec> iovecs,
                           const size_t offset, const uint8_t flags = 0,
                           const bool fixed_buffers = false,
                           const uint32_t buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot)
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const uint32_t index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_WRITEV_FIXED;
        else
            sqe->opcode = IORING_OP_WRITEV;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(iovecs.data());
        sqe->len = iovecs.size();
        sqe->off = offset;
        sqe->flags = flags;
        sqe->buf_index = buf_idx;

        return submit(slot, index);
    }

    PushResult push_read(const fd_t fd, const std::span<uint8_t> buff,
                         const size_t offset, const uint8_t flags = 0,
                         const bool fixed_buffers = false,
                         const uint32_t buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot)
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const uint32_t index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_READ_FIXED;
        else
            sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buff.data());
        sqe->len = buff.size();
        sqe->off = offset;
        sqe->flags = flags;
        sqe->buf_index = buf_idx;

        return submit(slot, index);
    }

    enum class PopError {
        CompletionCheckError,
        RingEmpty,
    };

    enum class WritevError {
        BadIovecsSize,
        AccessDenied,
        TemporarilyUnavailable,
        NoSpaceLeft,
        BadFd,
        WriteFailed,
        Terminated,
        Unexpected,
    };

    using PopWritevError = std::variant<PopError, WritevError>;

    std::expected<const uint32_t, PopWritevError> pop_writev() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value()) [[unlikely]]
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0) [[likely]]
            return static_cast<uint32_t>(rc);

        switch (io_uring_errno(rc)) {
        case Errno::SUCCESS:
            std::unreachable();
        case Errno::INVAL:
        case Errno::FBIG:
        case Errno::RANGE:
            return std::unexpected(WritevError::BadIovecsSize);
        case Errno::AGAIN:
        case Errno::DQUOT:
            return std::unexpected(WritevError::TemporarilyUnavailable);
        case Errno::BADF:
        case Errno::PIPE:
        case Errno::DESTADDRREQ:
        case Errno::NETDOWN:
        case Errno::NETUNREACH:
            return std::unexpected(WritevError::BadFd);
        case Errno::INTR:
            return std::unexpected(WritevError::Terminated);
        case Errno::NOSPC:
            return std::unexpected(WritevError::NoSpaceLeft);
        case Errno::NXIO:
            return std::unexpected(WritevError::WriteFailed);
        case Errno::ACCES:
            return std::unexpected(WritevError::AccessDenied);
        default:
            debug_e_errno(-rc);
            return std::unexpected(WritevError::Unexpected);
        }
    }

    enum class WriteError {
        BadBufferSize,
        AccessDenied,
        TemporarilyUnavailable,
        NoSpaceLeft,
        BadFd,
        WriteFailed,
        Terminated,
        Unexpected,
    };

    using PopWriteError = std::variant<PopError, WriteError>;

    std::expected<const uint32_t, PopWriteError> pop_write() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value()) [[unlikely]]
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0) [[likely]]
            return static_cast<uint32_t>(rc);

        switch (io_uring_errno(rc)) {
        case Errno::SUCCESS:
            std::unreachable();
        case Errno::INVAL:
        case Errno::FBIG:
        case Errno::RANGE:
            return std::unexpected(WriteError::BadBufferSize);
        case Errno::AGAIN:
        case Errno::DQUOT:
            return std::unexpected(WriteError::TemporarilyUnavailable);
        case Errno::BADF:
        case Errno::PIPE:
        case Errno::DESTADDRREQ:
        case Errno::NETDOWN:
        case Errno::NETUNREACH:
            return std::unexpected(WriteError::BadFd);
        case Errno::INTR:
            return std::unexpected(WriteError::Terminated);
        case Errno::NOSPC:
            return std::unexpected(WriteError::NoSpaceLeft);
        case Errno::NXIO:
            return std::unexpected(WriteError::WriteFailed);
        case Errno::ACCES:
            return std::unexpected(WriteError::AccessDenied);
        default:
            debug_e_errno(-rc);
            return std::unexpected(WriteError::Unexpected);
        }
    }

    enum class ReadError {
        TemporarilyUnavailable,
        BadFd,
        FdIsDir,
        BadBuffer,
        Unexpected,
    };

    using PopReadError = std::variant<PopError, ReadError>;

    std::expected<const uint32_t, PopReadError> pop_read() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value())
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0)
            return static_cast<uint32_t>(rc);

        switch (io_uring_errno(rc)) {
        case Errno::SUCCESS:
            std::unreachable();
        case Errno::AGAIN:
            return std::unexpected(ReadError::TemporarilyUnavailable);
        case Errno::BADF:
            return std::unexpected(ReadError::BadFd);
        case Errno::INVAL:
            return std::unexpected(ReadError::BadBuffer);
        case Errno::ISDIR:
            return std::unexpected(ReadError::FdIsDir);
        default:
            debug_e_errno(-rc);
            return std::unexpected(ReadError::Unexpected);
        }
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
    std::optional<fd_t> registered_ring_fd;

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
          cq_mask(cq_mask), cqes(cqes), is_sq_poll(is_sq_poll),
          registered_ring_fd(std::nullopt) {}

    // aggressive non-idle iouring thread
    //     params.sq_thread_idle = 0;
    // params.sq_thread_cpu = logger_io_cpu;

    void unregister_ring_fd() noexcept {
        if (registered_ring_fd.has_value()) {
            io_uring_rsrc_update reg{
                .offset = static_cast<uint32_t>(registered_ring_fd.value()),
                .resv = 0,
                .data = 0,
            };

            assert(io_uring_register(ring_fd.get(), IORING_UNREGISTER_RING_FDS,
                                     &reg, 1)
                       .has_value());
        }
    }

    // Should only be called during init
    void register_ring_fd() noexcept {
        const io_uring_rsrc_update reg{
            .offset = static_cast<uint32_t>(-1),
            .resv = 0,
            .data = static_cast<uint64_t>(ring_fd.get()),
        };
        const auto reg_r =
            io_uring_register(ring_fd.get(), IORING_REGISTER_RING_FDS, &reg, 1);
        if (reg_r.has_value())
            registered_ring_fd = static_cast<fd_t>(reg.offset);
    }

    uint32_t extra_enter_flags() noexcept {
        if (registered_ring_fd.has_value())
            return IORING_ENTER_REGISTERED_RING;
        return 0;
    }

    fd_t get_ring_fd() noexcept {
        if (registered_ring_fd)
            return registered_ring_fd.value();
        else
            return ring_fd.get();
    }

    // TODO: add draining
    std::expected<const int32_t, PopError> pop_one() {
        auto wait_r = io_uring_enter(
            get_ring_fd(), 0, 1, IORING_ENTER_GETEVENTS | extra_enter_flags());
        if (!wait_r.has_value()) [[unlikely]]
            return std::unexpected(PopError::CompletionCheckError);

        const uint32_t head = cq_head->load(std::memory_order_acquire);
        if (head == cq_tail->load(std::memory_order_relaxed)) [[unlikely]]
            return std::unexpected(PopError::RingEmpty);

        const int32_t rc = cqes[head & cq_mask].res;
        cq_head->store(head + 1, std::memory_order_release);
        return rc;
    }

    std::optional<uint32_t> next_sq_slot() {
        const uint32_t tail = sq_tail->load(std::memory_order_relaxed);
        const uint32_t head = sq_head->load(std::memory_order_acquire);

        if (tail - head >= (sq_mask + 1)) [[unlikely]]
            return std::nullopt;
        return std::optional(tail);
    }

    PushResult submit(const uint32_t slot, const uint32_t index) noexcept {
        sq_array[index] = index;
        // This way of doing the stamping and not claiming the slot earlier
        // and having no checks over whats stored clearly means it's not thread
        // safe
        sq_tail->store(slot + 1, std::memory_order_release);

        if (is_sq_poll && (*sq_flags & IORING_SQ_NEED_WAKEUP)) [[unlikely]] {
            auto r =
                io_uring_enter(get_ring_fd(), 0, 0,
                               IORING_ENTER_SQ_WAKEUP | extra_enter_flags());
            if (!r.has_value()) [[unlikely]]
                return PushResult::WakeFailed;
        } else {
            auto r =
                io_uring_enter(get_ring_fd(), 1, 0, 0 | extra_enter_flags());
            if (!r.has_value()) [[unlikely]]
                return PushResult::WakeFailed;
        }

        return PushResult::Success;
    }
};
} // namespace shclog::io::iouring
