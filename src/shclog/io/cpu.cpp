#include "shclog/io/cpu.hpp"
#include "shclog/io/syscall.hpp"
#include "shclog/types.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/openat2.h>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <utility>

namespace shclog::io::cpu {

using namespace shclog::io::syscall;

SetCpuAffinityResult _set_cpu_affinity(const cpu_set_t &set,
                                       const pthread_t thread_target) noexcept {

    const i32 rc =
        pthread_setaffinity_np(thread_target, sizeof(cpu_set_t), &set);

    if (rc == 0) [[likely]]
        return SetCpuAffinityResult::Success;

    assert(std::in_range<u64>(rc));
    switch (from_errno(static_cast<u64>(rc))) {
    case Errno::SUCCESS:
        std::unreachable();
    case Errno::INVAL:
    case Errno::SRCH:
        return SetCpuAffinityResult::InvalidParam;
    default:
        debug_e_errno(rc);
        return SetCpuAffinityResult::Unexpected;
    }
    std::unreachable();
}

SetCpuAffinityResult
set_cpu_afinity(const usize cpu,
                const std::optional<const pthread_t> target) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    return _set_cpu_affinity(set, pthread_t_uwrap(target));
}
} // namespace shclog::io::cpu
