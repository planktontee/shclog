#pragma once

#include "shclog/types.hpp"
#include <expected>
#include <fcntl.h>
#include <span>
#include <unistd.h>
#include <utility>

namespace shclog::io::file {
using fd_t = c_int;
constexpr fd_t invalid_fd = -1;

class unique_fd {
  public:
    explicit unique_fd(fd_t fd = -1) noexcept : fd(fd) {}

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

    [[nodiscard]] fd_t get() const noexcept { return fd; }

    fd_t release() noexcept { return std::exchange(fd, -1); }

    void reset(fd_t fd = -1) noexcept {
        if (this->fd != -1)
            close(this->fd);

        this->fd = fd;
    }

    explicit operator bool() const noexcept { return fd != -1; }

  private:
    fd_t fd;
};

enum OpenError : u8 {
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

enum class OpenMode : u8 {
    read = O_RDONLY,
    write = O_WRONLY,
    read_write = O_RDWR,
};

std::expected<unique_fd, OpenError>
tmpfile(const OpenMode openMode = OpenMode::read_write,
        const u64 flags = O_CLOEXEC, const u64 mode = 0600) noexcept;

// TODO: unroll errors
enum class WriteError : u8 {
    Unexpected,
};

std::expected<u64, WriteError>
pwrite64(const fd_t fd, const std::span<const u8> buf, const i64 offset);

// TODO: unroll errors
enum class WritevError : u8 {
    Unexpected,
};

std::expected<u64, WritevError>
pwritev(const fd_t fd, const std::span<const iovec> buf, const i64 offset);

enum class ReadError : u8 {
    TemporarilyUnavailable,
    BadFd,
    FdIsDir,
    BadBuffer,
    NonSeekableFd,
    InvalidParams,
    Terminated,
    Unexpected,
};

std::expected<u64, ReadError> pread(const fd_t fd, const std::span<u8> buf,
                                    const i64 offset);
} // namespace shclog::io::file
