#pragma once

#include "shclog/io/file.hpp"
#include "shclog/io/syscall.hpp"
#include <expected>
#include <memory>
#include <sys/mman.h>
#include <sys/types.h>

namespace shclog::io::mmap {
using namespace shclog::io::syscall;
using namespace shclog::io::file;
struct MmapDeleter {
    size_t size;
    void operator()(void *const ptr) const {
        if (ptr)
            munmap(ptr, size);
    }
};

enum MmapError : uint8_t {
    AccessDenied,
    PermissionDenied,
    LockedMemoryLimitExceeded,
    BadFileDescriptor,
    MemoryMappingNotSupported,
    ProcessFdQuotaExceeded,
    SystemFdQuotaExceeded,
    OutOfMemory,
    MappingAlreadyExists,
    Unexpected,
};

template <typename R>
std::expected<std::unique_ptr<R, MmapDeleter>, MmapError>
mmap(const size_t len, void *addr = nullptr,
     const int prot = PROT_READ | PROT_WRITE,
     const int flags = MAP_PRIVATE | MAP_ANONYMOUS, const fd_t fd = invalid_fd,
     int64_t offset = 0) {

    const size_t total_bytes = len * sizeof(R);
    void *const rc = ::mmap(addr, total_bytes, prot, flags, fd, offset);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    case Errno::ACCES:
    case Errno::TXTBSY:
        return std::unexpected(MmapError::AccessDenied);
    case Errno::PERM:
        return std::unexpected(MmapError::PermissionDenied);
    case Errno::AGAIN:
        return std::unexpected(MmapError::LockedMemoryLimitExceeded);
    case Errno::BADF:
        return std::unexpected(MmapError::BadFileDescriptor);
    case Errno::NODEV:
        return std::unexpected(MmapError::MemoryMappingNotSupported);
    case Errno::MFILE:
        return std::unexpected(MmapError::ProcessFdQuotaExceeded);
    case Errno::NFILE:
        return std::unexpected(MmapError::SystemFdQuotaExceeded);
    case Errno::NOMEM:
        return std::unexpected(MmapError::OutOfMemory);
    case Errno::EXIST:
        return std::unexpected(MmapError::MappingAlreadyExists);
    default:
        debug_e_errno();
        return std::unexpected(MmapError::Unexpected);
    }

    return std::unique_ptr<R, MmapDeleter>(static_cast<R *const>(rc),
                                           MmapDeleter(total_bytes));
}

void munmap(void *const ptr, const size_t size) noexcept;
} // namespace shclog::io::mmap
