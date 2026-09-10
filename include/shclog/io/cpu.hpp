#include <cstddef>
#include <optional>
#include <ranges>
#include <sys/types.h>

namespace shclog::io::cpu {

enum SetCpuAffinityResult {
    Success,
    InvalidParam,
    Unexpected,
};

SetCpuAffinityResult set_cpu_afinity(
    const size_t cpu,
    const std::optional<const pthread_t> target = std::nullopt) noexcept;

SetCpuAffinityResult set_cpu_afinity(
    std::ranges::iota_view<size_t, size_t> range,
    const std::optional<const pthread_t> target = std::nullopt) noexcept;
} // namespace shclog::io::cpu
