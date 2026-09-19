#include "shclog/io/file.hpp"
#include "shclog/io/syscall.hpp"
#include "shclog/types.hpp"
#include <asm/unistd_64.h>
#include <expected>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace shclog::io::file {
using namespace shclog::io::syscall;

void close(fd_t fd) noexcept {
    const c_int rc = ::close(fd);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    default:
        debug_e_errno();
        break;
    }
}

std::expected<fd_t, OpenError>
open(const char *const path, const ::open_how *const how, fd_t cwd) noexcept {
    constexpr const usize size = sizeof(::open_how);
    const i64 rc = ::syscall(SYS_openat2, cwd, path, how, size);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::ACCES:
    case Errno::PERM:
    case Errno::ROFS:
        return std::unexpected(OpenError::AccessDenied);
    case Errno::BADF:
        return std::unexpected(OpenError::BadCwd);
    case Errno::DQUOT:
    case Errno::MFILE:
        return std::unexpected(OpenError::QuotaExceeded);
    case Errno::EXIST:
        return std::unexpected(OpenError::FileAlreadyExists);
    case Errno::AGAIN:
    case Errno::BUSY:
    case Errno::TXTBSY:
        if (how->resolve & RESOLVE_CACHED)
            return std::unexpected(OpenError::RetryableWithoutCache);
        return std::unexpected(OpenError::TemporarilyUnavailable);
    case Errno::LOOP:
        return std::unexpected(OpenError::PathContainsLink);
    case Errno::XDEV:
        return std::unexpected(OpenError::PathCrossesMount);
    case Errno::INVAL:
    case Errno::TOOBIG:
    case Errno::FAULT:
        return std::unexpected(OpenError::InvalidParams);
    case Errno::OVERFLOW:
    case Errno::FBIG:
        return std::unexpected(OpenError::FileIsTooBig);
    case Errno::INTR:
        return std::unexpected(OpenError::Terminated);
    case Errno::ISDIR:
        return std::unexpected(OpenError::FdIsDir);
    case Errno::NAMETOOLONG:
        return std::unexpected(OpenError::PathIsTooLong);
    case Errno::NFILE:
        return std::unexpected(OpenError::SystemQuotaExceeded);
    case Errno::NOENT:
    case Errno::NXIO:
        return std::unexpected(OpenError::FileNotFound);
    case Errno::NOMEM:
        return std::unexpected(OpenError::OutOfMemory);
    case Errno::NOSPC:
        return std::unexpected(OpenError::OutOfSpace);
    case Errno::NOTDIR:
        return std::unexpected(OpenError::FdIsNotADir);
    default:
        debug_e_errno();
        return std::unexpected(OpenError::Unexpected);
    }

    return static_cast<fd_t>(rc);
}

// if someone gets mad about the cwd getting littered, we can add path/dir_fd
// here
// dont ask why mode is u64, idk either
std::expected<unique_fd, OpenError>
tmpfile(const OpenMode openMode, const u64 flags, const u64 mode) noexcept {
    open_how how{};
    how.flags = O_TMPFILE | static_cast<u64>(openMode) | flags;
    how.mode = mode;
    auto open_r = open(".", &how);
    if (!open_r.has_value())
        return std::unexpected(open_r.error());

    return std::expected<unique_fd, OpenError>(std::in_place,
                                               unique_fd(open_r.value()));
}

std::expected<u64, WriteError>
pwrite64(const fd_t fd, const std::span<const u8> buf, const i64 offset) {
    const i64 rc = ::syscall(__NR_pwrite64, fd, buf.data(), buf.size(), offset);

    // TODO: unroll errors
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    default:
        debug_e_errno();
        return std::unexpected(WriteError::Unexpected);
    }

    return static_cast<u64>(rc);
}

std::expected<u64, WritevError>
pwritev(const fd_t fd, const std::span<const iovec> buf, const i64 offset) {
    const i64 rc = ::syscall(__NR_pwritev, fd, buf.data(), buf.size(), offset);

    // TODO: unroll errors
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    default:
        debug_e_errno();
        return std::unexpected(WritevError::Unexpected);
    }

    return static_cast<u64>(rc);
}

std::expected<u64, ReadError> pread(const fd_t fd, const std::span<u8> buf,
                                    const i64 offset) {
    const i64 rc = ::syscall(__NR_pread64, fd, buf.data(), buf.size(), offset);

    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::FAULT:
        return std::unexpected(ReadError::BadBuffer);
    case Errno::BADF:
        return std::unexpected(ReadError::BadFd);
    case Errno::AGAIN:
        return std::unexpected(ReadError::TemporarilyUnavailable);
    case Errno::SPIPE:
        return std::unexpected(ReadError::NonSeekableFd);
    case Errno::INTR:
        return std::unexpected(ReadError::Terminated);
    case Errno::ISDIR:
        return std::unexpected(ReadError::FdIsDir);
    case Errno::INVAL:
    case Errno::NXIO:
    case Errno::OPNOTSUPP:
    case Errno::OVERFLOW:
        return std::unexpected(ReadError::InvalidParams);
    default:
        debug_e_errno();
        return std::unexpected(ReadError::Unexpected);
    }

    return static_cast<u64>(rc);
}
} // namespace shclog::io::file
