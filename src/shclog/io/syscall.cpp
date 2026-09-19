#include "shclog/io/syscall.hpp"
#include "shclog/const.hpp"
#include "shclog/types.hpp"
#include <bit>
#include <cstring>
#include <print>

namespace shclog::io::syscall {
inline bool _errno_out_of_bounds(const i64 e) noexcept {
    return e < 0 || e > 177;
}

Errno debug_e_errno(const i32 e_errno,
                    const std::source_location loc) noexcept {
    if constexpr (IS_DEBUG) {
        std::println(stderr, "Panic in {}: {} (errno: {})", loc.function_name(),
                     std::strerror(e_errno), e_errno);
        std::abort();
    }
    return Errno::UNEXPECTED;
}

Errno e_errno(const i64 rc) noexcept {
    if (rc >= 0)
        return Errno::SUCCESS;

    const c_int e = errno;

    if (_errno_out_of_bounds(e))
        return Errno::UNEXPECTED;

    return static_cast<Errno>(e);
}

Errno io_uring_errno(const i64 rc) noexcept {
    if (rc >= 0)
        return Errno::SUCCESS;

    const auto e = -rc;

    // last defined Errno comparison
    if (_errno_out_of_bounds(e))
        return Errno::UNEXPECTED;

    return static_cast<Errno>(e);
}

Errno from_errno(const u64 rc) noexcept {
    if (rc == 0)
        return Errno::SUCCESS;

    if (rc > 177)
        return Errno::UNEXPECTED;

    return static_cast<Errno>(rc);
}

Errno e_errno(const void *rc) noexcept {
    if (std::bit_cast<isize>(rc) != -1)
        return Errno::SUCCESS;

    return e_errno(-1);
}

} // namespace shclog::io::syscall
