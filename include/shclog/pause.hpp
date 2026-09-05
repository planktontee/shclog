#include <emmintrin.h>

namespace shclog::pause {
template <int N> inline void mm_pause() {
    for (int i = 0; i < N; ++i)
        _mm_pause();
}
} // namespace shclog::pause
