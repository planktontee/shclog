#include "shclog/io/iouring.hpp"
#include <asm/unistd_64.h>

namespace shclog::io::iouring {

const std::expected<const fd_t, IoUringSetupError>
io_uring_setup(io_uring_params &params, const uint32_t capacity) noexcept {
    const int rc = ::syscall(__NR_io_uring_setup, capacity, &params);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::MFILE:
        return std::unexpected(IoUringSetupError::ProcessFdQuotaExceeded);
    case Errno::NFILE:
        return std::unexpected(IoUringSetupError::SystemFdQuotaExceeded);
    case Errno::NOMEM:
        return std::unexpected(IoUringSetupError::OutOfMemory);
    case Errno::PERM:
        return std::unexpected(IoUringSetupError::PermissionDenied);
    case Errno::NXIO:
        return std::unexpected(IoUringSetupError::NoSuchFd);
    default:
        debug_e_errno();
        return std::unexpected(IoUringSetupError::Unexpected);
    }

    return static_cast<fd_t>(rc);
}

const std::expected<const uint32_t, IoUringEnterError>
io_uring_enter(const fd_t ring_fd, const uint32_t to_submit,
               const uint32_t min_complete, const uint32_t flags,
               sigset_t *sig) noexcept {

    const int rc = ::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                             min_complete, flags, sig);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::AGAIN:
        return std::unexpected(
            IoUringEnterError::ResourcesTemporarilyUnavailable);
    case Errno::BADFD:
        return std::unexpected(IoUringEnterError::RingIsDisabled);
    case Errno::BADR:
        return std::unexpected(IoUringEnterError::CompletionQueueIsFull);
    case Errno::BUSY:
        return std::unexpected(IoUringEnterError::SubmissionQueueIsFull);
    default:
        debug_e_errno();
        return std::unexpected(IoUringEnterError::Unexpected);
    }

    return static_cast<uint32_t>(rc);
}

} // namespace shclog::io::iouring
