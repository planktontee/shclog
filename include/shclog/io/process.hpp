#pragma once

#include "shclog/types.hpp"
#include <optional>
#include <pthread.h>
namespace shclog::io::process {

enum SetPriorityResult : u8 {
    Success,
    InvalidParam,
    AccessDenied,
    Unexpected,
};

pthread_t pthread_t_uwrap(const std::optional<const pthread_t> target) noexcept;

SetPriorityResult
set_priority(const i32 niceness,
             const std::optional<pid_t> target = std::nullopt) noexcept;
} // namespace shclog::io::process
