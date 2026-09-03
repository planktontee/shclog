#include "shclog/io/syscall.hpp"
#include <cerrno>
#include <cstring>
#include <print>
#include <utility>

namespace shclog::io::syscall {
Errno debug_e_errno(const std::source_location loc) noexcept {
#ifndef NDEBUG
    std::println(stderr, "Panic in {}: {} (errno: {})", loc.function_name(),
                 std::strerror(errno), errno);
    std::abort();
    std::unreachable();
#else
    return Errno::UNEXPECTED;
#endif
}

Errno e_errno(const int rc) noexcept {
    if (rc >= 0)
        return Errno::SUCCESS;

    const int e = errno;

    // last defined Errno comparison
    if (e < 0 || e > 177)
        return Errno::UNEXPECTED;

    return static_cast<Errno>(e);
}

Errno e_errno(const void *rc) noexcept {
    if (rc != reinterpret_cast<void *>(-1))
        return Errno::SUCCESS;

    return e_errno(-1);
}

} // namespace shclog::io::syscall
