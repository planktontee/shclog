#include "shclog/io/process.hpp"
#include "shclog/cast.hpp"
#include "shclog/io/syscall.hpp"
#include "shclog/types.hpp"
#include <optional>
#include <pthread.h>
#include <sys/resource.h>
#include <utility>

namespace shclog::io::process {

using namespace shclog::io::syscall;

pthread_t
pthread_t_uwrap(const std::optional<const pthread_t> target) noexcept {
    if (target.has_value())
        return *target;
    return pthread_self();
}

SetPriorityResult set_priority(const i32 niceness,
                               const std::optional<pid_t> target) noexcept {
    const auto rc =
        setpriority(PRIO_PROCESS, int_cast(target.value_or(0)), niceness);

    if (rc >= 0) [[likely]]
        return SetPriorityResult::Success;

    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        std::unreachable();
    case Errno::SRCH:
        return SetPriorityResult::InvalidParam;
    case Errno::ACCES:
    case Errno::PERM:
        return SetPriorityResult::AccessDenied;
    default:
        debug_e_errno();
        return SetPriorityResult::Unexpected;
    }
    std::unreachable();
}
} // namespace shclog::io::process
