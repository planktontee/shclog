#include "shclog/io/iouring.hpp"
#include "shclog/types.hpp"
#include <asm/unistd_64.h>
#include <utility>

namespace shclog::io::iouring {

const std::expected<const fd_t, IoUringSetupError>
io_uring_setup(io_uring_params &params, const u32 capacity) noexcept {
    const i64 rc = ::syscall(__NR_io_uring_setup, capacity, &params);
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

const std::expected<const u32, IoUringEnterError>
io_uring_enter(const fd_t ring_fd, const u32 to_submit, const u32 min_complete,
               const u32 flags, sigset_t *sig) noexcept {

    const i64 rc = ::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                             min_complete, flags, sig);
    if (rc < 0) [[unlikely]]
        switch (e_errno(rc)) {
        case Errno::SUCCESS:
            std::unreachable();
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

    return static_cast<u32>(rc);
}

const std::expected<const u32, IoUringRegisterError>
io_uring_register(const fd_t ring_fd, const u32 opcode, const void *arg,
                  u32 nr_args) noexcept {

    const i64 rc =
        ::syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);

    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::INVAL:
    case Errno::FAULT:
    case Errno::OPNOTSUPP:
    case Errno::NOENT:
        return std::unexpected(IoUringRegisterError::BadRequest);
    case Errno::ACCES:
    case Errno::EXIST:
        return std::unexpected(IoUringRegisterError::AccessDenied);
    case Errno::BADF:
        return std::unexpected(IoUringRegisterError::BadFd);
    case Errno::BUSY:
        return std::unexpected(
            IoUringRegisterError::RegistrationsAlreadyInPlace);
    case Errno::MFILE:
        return std::unexpected(IoUringRegisterError::TooManyFiles);
    case Errno::NOMEM:
        return std::unexpected(IoUringRegisterError::OutOfMem);
    case Errno::NXIO:
        return std::unexpected(IoUringRegisterError::RegistrationFailed);
    default:
        debug_e_errno();
        return std::unexpected(IoUringRegisterError::Unexpected);
    }

    return static_cast<u32>(rc);
}
} // namespace shclog::io::iouring
