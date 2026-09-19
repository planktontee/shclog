#pragma once

#include "shclog/types.hpp"
#include <emmintrin.h>

namespace shclog::pause {
template <usize N> inline void mm_pause() {
    for (usize i = 0; i < N; ++i)
        _mm_pause();
}
} // namespace shclog::pause
