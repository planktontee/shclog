#include <cstdint>
#include <optional>
#include <pthread.h>
namespace shclog::io::process {

enum SetPriorityResult {
    Success,
    InvalidParam,
    AccessDenied,
    Unexpected,
};

pthread_t pthread_t_uwrap(const std::optional<const pthread_t> target) noexcept;

SetPriorityResult
set_priority(const int32_t niceness,
             const std::optional<pthread_t> target = std::nullopt) noexcept;
} // namespace shclog::io::process
