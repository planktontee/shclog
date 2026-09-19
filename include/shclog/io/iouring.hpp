#pragma once

#include "shclog/cast.hpp"
#include "shclog/io/mmap.hpp"
#include "shclog/types.hpp"
#include <algorithm>
#include <atomic>
#include <bits/types/sigset_t.h>
#include <cassert>
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

enum class IoUringSetupError : u8 {
    NoSuchFd,
    PermissionDenied,
    ProcessFdQuotaExceeded,
    SystemFdQuotaExceeded,
    OutOfMemory,
    Unexpected,
};

const std::expected<const fd_t, IoUringSetupError>
io_uring_setup(io_uring_params &params, const u32 capacity) noexcept;

enum class IoUringEnterError : u8 {
    ResourcesTemporarilyUnavailable,
    BadFdForRing,
    RingIsDisabled,
    CompletionQueueIsFull,
    SubmissionQueueIsFull,
    Unexpected,
};

const std::expected<const u32, IoUringEnterError>
io_uring_enter(const fd_t ring_fd, const u32 to_submit, const u32 min_complete,
               const u32 flags, sigset_t *sig = nullptr) noexcept;

enum class IoUringRegisterError : u8 {
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

const std::expected<const u32, IoUringRegisterError>
io_uring_register(const fd_t ring_fd, const u32 opcode, const void *arg,
                  u32 nr_args) noexcept;

enum class RingQueueAllocError : u8 {
    OutOfMemory,
    Unexpected,
};

template <typename T = u8>
static std::expected<std::unique_ptr<T, MmapDeleter>, RingQueueAllocError>
io_uring_mmap(const fd_t ring_fd, const usize size, const u64 offset) {

    auto mmp_r =
        mmap::mmap<T>(size, nullptr, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, ring_fd, int_cast(offset));

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
    enum CreateError : u8 {
        InvalidCapacity,
        UnableToSetupRing,
        NoAvailableFd,
        OutOfMemory,
    };

    EventedIo(const EventedIo &) = delete;
    EventedIo &operator=(const EventedIo &) = delete;
    EventedIo(EventedIo &&) = delete;
    EventedIo &operator=(EventedIo &&) = delete;

    ~EventedIo() noexcept {
        [[maybe_unused]] const auto unreg_buf_r = io_uring_register(
            ring_fd.get(), IORING_UNREGISTER_BUFFERS, nullptr, 0);
        assert(unreg_buf_r.has_value());
        [[maybe_unused]] const auto unreg_f_r = io_uring_register(
            ring_fd.get(), IORING_UNREGISTER_FILES, nullptr, 0);
        assert(unreg_f_r.has_value());

        unregister_ring_fd();
    }

    static std::expected<std::unique_ptr<EventedIo>, CreateError>
    create(const u32 capacity, const u32 flags) noexcept {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            return std::unexpected(CreateError::InvalidCapacity);

        io_uring_params params{};
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

        usize sq_size = params.sq_off.array + params.sq_entries * sizeof(u32);
        usize cq_size =
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
        std::unique_ptr<u8, MmapDeleter> cq_ptr = nullptr;

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

        std::atomic<u32> *const sq_head =
            ptr_cast<std::atomic<u32>>(sq_ptr.get() + params.sq_off.head);
        std::atomic<u32> *const sq_tail =
            ptr_cast<std::atomic<u32>>(sq_ptr.get() + params.sq_off.tail);

        u32 *const sq_flags = ptr_cast<u32>(sq_ptr.get() + params.sq_off.flags);
        u32 *const sq_array = ptr_cast<u32>(sq_ptr.get() + params.sq_off.array);
        u32 *const sq_dropped =
            ptr_cast<u32>(sq_ptr.get() + params.sq_off.dropped);
        const u32 sq_mask =
            *ptr_cast<u32>(sq_ptr.get() + params.sq_off.ring_mask);

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

        u8 *const cq_ptr_raw = cq_ptr ? cq_ptr.get() : sq_ptr.get();

        std::atomic<u32> *const cq_head =
            ptr_cast<std::atomic<u32>>(cq_ptr_raw + params.cq_off.head);
        std::atomic<u32> *const cq_tail =
            ptr_cast<std::atomic<u32>>(cq_ptr_raw + params.cq_off.tail);

        u32 *const cq_flags = ptr_cast<u32>(cq_ptr_raw + params.cq_off.flags);
        u32 *const cq_overflow =
            ptr_cast<u32>(cq_ptr_raw + params.cq_off.overflow);
        const u32 cq_mask =
            *ptr_cast<u32>(cq_ptr_raw + params.cq_off.ring_mask);
        io_uring_cqe *const cqes =
            ptr_cast<io_uring_cqe>(cq_ptr_raw + params.cq_off.cqes);

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
        const auto reg_r =
            io_uring_register(ring_fd.get(), IORING_REGISTER_FILES,
                              reinterpret_cast<const void *>(files.data()),
                              int_cast(files.size()));
        if (!reg_r.has_value())
            return false;
        return true;
    }

    bool register_buffers(const std::span<const iovec> bufs) noexcept {
        const auto reg_r = io_uring_register(
            ring_fd.get(), IORING_REGISTER_BUFFERS,
            reinterpret_cast<const void *>(bufs.data()), int_cast(bufs.size()));
        if (!reg_r.has_value())
            return false;
        return true;
    }

    enum class PushResult : u8 {
        Success,
        WakeFailed,
        QueueIsFull,
    };

    PushResult push_write(const fd_t fd, const std::span<const u8> buf,
                          const usize offset, const u8 flags = 0,
                          const u32 rw_flags = 0,
                          const bool fixed_buffers = false,
                          const u32 buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot) [[unlikely]]
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const u32 index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_WRITE_FIXED;
        else
            sqe->opcode = IORING_OP_WRITE;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<u64>(buf.data());
        sqe->len = int_cast(buf.size());
        sqe->off = offset;
        sqe->flags = flags;
        if (fixed_buffers) {
            sqe->buf_index = int_cast(buf_idx);
        }
        sqe->rw_flags = rw_flags;

        return submit(slot, index);
    }

    // TODO: add rw_flags
    PushResult push_writev(const fd_t fd, const std::span<const iovec> iovecs,
                           const usize offset, const u8 flags = 0,
                           const bool fixed_buffers = false,
                           const u32 buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot)
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const u32 index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_WRITEV_FIXED;
        else
            sqe->opcode = IORING_OP_WRITEV;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<u64>(iovecs.data());
        sqe->len = int_cast(iovecs.size());
        sqe->off = offset;
        sqe->flags = flags;
        sqe->buf_index = int_cast(buf_idx);

        return submit(slot, index);
    }

    PushResult push_read(const fd_t fd, const std::span<u8> buff,
                         const usize offset, const u8 flags = 0,
                         const bool fixed_buffers = false,
                         const u32 buf_idx = 0) noexcept {
        const auto opt_slot = next_sq_slot();
        if (!opt_slot)
            return PushResult::QueueIsFull;
        const auto slot = opt_slot.value();

        const u32 index = slot & sq_mask;
        auto sqe = &sqes.get()[index];
        *sqe = {};
        if (fixed_buffers)
            sqe->opcode = IORING_OP_READ_FIXED;
        else
            sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<u64>(buff.data());
        sqe->len = int_cast(buff.size());
        sqe->off = offset;
        sqe->flags = flags;
        sqe->buf_index = int_cast(buf_idx);

        return submit(slot, index);
    }

    enum class PopError : u8 {
        CompletionCheckError,
        RingEmpty,
    };

    enum class WritevError : u8 {
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

    std::expected<const u32, PopWritevError> pop_writev() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value()) [[unlikely]]
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0) [[likely]]
            return static_cast<u32>(rc);

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

    enum class WriteError : u8 {
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

    std::expected<const u32, PopWriteError> pop_write() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value()) [[unlikely]]
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0) [[likely]]
            return static_cast<u32>(rc);

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

    enum class ReadError : u8 {
        TemporarilyUnavailable,
        BadFd,
        FdIsDir,
        BadBuffer,
        Unexpected,
    };

    using PopReadError = std::variant<PopError, ReadError>;

    std::expected<const u32, PopReadError> pop_read() noexcept {
        const auto rc_r = pop_one();
        if (!rc_r.has_value())
            return std::unexpected(rc_r.error());

        const auto rc = rc_r.value();
        if (rc >= 0)
            return static_cast<u32>(rc);

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

    std::unique_ptr<u8, MmapDeleter> sq_ptr;
    std::unique_ptr<u8, MmapDeleter> cq_ptr;

    std::atomic<u32> *const sq_head;
    std::atomic<u32> *const sq_tail;
    u32 *const sq_flags;
    u32 *const sq_array;
    [[maybe_unused]] u32 *const sq_dropped;
    const u32 sq_mask;
    std::unique_ptr<io_uring_sqe, MmapDeleter> sqes;

    std::atomic<u32> *const cq_head;
    std::atomic<u32> *const cq_tail;
    [[maybe_unused]] u32 *const cq_flags;
    [[maybe_unused]] u32 *const cq_overflow;
    const u32 cq_mask;
    io_uring_cqe *const cqes;

    const bool is_sq_poll;
    std::optional<fd_t> registered_ring_fd;

    EventedIo(unique_fd ring_fd,

              std::unique_ptr<u8, MmapDeleter> sq_ptr,
              std::unique_ptr<u8, MmapDeleter> cq_ptr,

              std::atomic<u32> *const sq_head, std::atomic<u32> *const sq_tail,
              u32 *const sq_flags, u32 *const sq_array, u32 *const sq_dropped,
              const u32 sq_mask,
              std::unique_ptr<io_uring_sqe, MmapDeleter> sqes,

              std::atomic<u32> *const cq_head, std::atomic<u32> *const cq_tail,
              u32 *const cq_flags, u32 *const cq_overflow, const u32 cq_mask,
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
                .offset = static_cast<u32>(registered_ring_fd.value()),
                .resv = 0,
                .data = 0,
            };

            [[maybe_unused]] const auto unreg_ring_r = io_uring_register(
                ring_fd.get(), IORING_UNREGISTER_RING_FDS, &reg, 1);
            assert(unreg_ring_r.has_value());
        }
    }

    // Should only be called during init
    void register_ring_fd() noexcept {
        const io_uring_rsrc_update reg{
            .offset = std::bit_cast<u32>(-1),
            .resv = 0,
            .data = int_cast(ring_fd.get()),
        };
        const auto reg_r =
            io_uring_register(ring_fd.get(), IORING_REGISTER_RING_FDS, &reg, 1);
        if (reg_r.has_value())
            registered_ring_fd = int_cast(reg.offset);
    }

    u32 extra_enter_flags() noexcept {
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
    std::expected<const i32, PopError> pop_one() {
        auto wait_r = io_uring_enter(
            get_ring_fd(), 0, 1, IORING_ENTER_GETEVENTS | extra_enter_flags());
        if (!wait_r.has_value()) [[unlikely]]
            return std::unexpected(PopError::CompletionCheckError);

        const u32 head = cq_head->load(std::memory_order_acquire);
        if (head == cq_tail->load(std::memory_order_relaxed)) [[unlikely]]
            return std::unexpected(PopError::RingEmpty);

        const i32 rc = cqes[head & cq_mask].res;
        cq_head->store(head + 1, std::memory_order_release);
        return rc;
    }

    std::optional<u32> next_sq_slot() {
        const u32 tail = sq_tail->load(std::memory_order_relaxed);
        const u32 head = sq_head->load(std::memory_order_acquire);

        if (tail - head >= (sq_mask + 1)) [[unlikely]]
            return std::nullopt;
        return std::optional(tail);
    }

    PushResult submit(const u32 slot, const u32 index) noexcept {
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
