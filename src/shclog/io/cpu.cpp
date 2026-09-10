
#include "shclog/io/cpu.hpp"
#include "shclog/io/process.hpp"
#include "shclog/io/syscall.hpp"
#include <optional>
#include <pthread.h>
#include <ranges>
#include <sched.h>
#include <sys/types.h>
#include <utility>

namespace shclog::io::cpu {

using namespace shclog::io::syscall;
using namespace shclog::io::process;

SetCpuAffinityResult _set_cpu_affinity(const cpu_set_t &set,
                                       const pthread_t thread_target) noexcept {
    const auto rc =
        pthread_setaffinity_np(thread_target, sizeof(cpu_set_t), &set);

    if (rc >= 0) [[likely]]
        return SetCpuAffinityResult::Success;

    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        std::unreachable();
    case Errno::INVAL:
    case Errno::SRCH:
        return SetCpuAffinityResult::InvalidParam;
    default:
        debug_e_errno();
        return SetCpuAffinityResult::Unexpected;
    }
    std::unreachable();
}

SetCpuAffinityResult
set_cpu_afinity(const size_t cpu,
                const std::optional<const pthread_t> target) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    return _set_cpu_affinity(set, pthread_t_uwrap(target));
}

SetCpuAffinityResult
set_cpu_afinity(std::ranges::iota_view<size_t, size_t> range,
                const std::optional<const pthread_t> target) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const size_t cpu : range)
        CPU_SET(cpu, &set);

    return _set_cpu_affinity(set, pthread_t_uwrap(target));
}
} // namespace shclog::io::cpu
