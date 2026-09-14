#pragma once

#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <span>
#include <unistd.h>
#include <utility>

namespace shclog::io::file {
using fd_t = int;
constexpr fd_t invalid_fd = -1;

class unique_fd {
  public:
    explicit unique_fd(int fd = -1) noexcept : fd(fd) {}

    ~unique_fd() noexcept {
        if (fd != -1)
            close(fd);
    }

    unique_fd(const unique_fd &) = delete;
    unique_fd &operator=(const unique_fd &) = delete;

    unique_fd(unique_fd &&other) noexcept : fd(std::exchange(other.fd, -1)) {}

    unique_fd &operator=(unique_fd &&other) noexcept {
        if (this != &other) {
            reset();
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }

    int get() const noexcept { return fd; }

    int release() noexcept { return std::exchange(fd, -1); }

    void reset(int fd = -1) noexcept {
        if (this->fd != -1)
            close(this->fd);

        this->fd = fd;
    }

    explicit operator bool() const noexcept { return fd != -1; }

  private:
    fd_t fd;
};

enum OpenError {
    RetryableWithoutCache,
    PathContainsLink,
    PathCrossesMount,
    InvalidParams,
    AccessDenied,
    TemporarilyUnavailable,
    QuotaExceeded,
    SystemQuotaExceeded,
    FileAlreadyExists,
    FileNotFound,
    OutOfMemory,
    OutOfSpace,
    FdIsNotADir,
    FileIsTooBig,
    Terminated,
    FdIsDir,
    PathIsTooLong,
    BadCwd,
    Unexpected,
};

void close(fd_t fd) noexcept;
std::expected<fd_t, OpenError> open(const char *const path,
                                    const ::open_how *const how,
                                    fd_t cwd = AT_FDCWD) noexcept;

enum class OpenMode {
    read = O_RDONLY,
    write = O_WRONLY,
    read_write = O_RDWR,
};

std::expected<unique_fd, OpenError>
tmpfile(const OpenMode openMode = OpenMode::read_write,
        const uint64_t flags = O_CLOEXEC, const uint64_t mode = 0600) noexcept;

// TODO: unroll errors
enum class WriteError {
    Unexpected,
};

std::expected<uint64_t, WriteError> pwrite64(const fd_t fd,
                                             const std::span<const uint8_t> buf,
                                             const int64_t offset);

// TODO: unroll errors
enum class WritevError {
    Unexpected,
};

std::expected<uint64_t, WritevError> pwritev(const fd_t fd,
                                             const std::span<const iovec> buf,
                                             const int64_t offset,
                                             const int32_t flags = 0);
enum class ReadError {
    TemporarilyUnavailable,
    BadFd,
    FdIsDir,
    BadBuffer,
    NonSeekableFd,
    InvalidParams,
    Terminated,
    Unexpected,
};

std::expected<uint64_t, ReadError>
pread(const fd_t fd, const std::span<uint8_t> buf, const int64_t offset);
} // namespace shclog::io::file
