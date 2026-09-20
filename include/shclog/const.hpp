#pragma once

namespace shclog {
#if defined(NDEBUG)
inline constexpr bool IS_DEBUG = false;
#else
inline constexpr bool IS_DEBUG = true;
#endif

#ifdef __SANITIZE_THREAD__
inline constexpr bool IS_TSAN = true;
#else
inline constexpr bool IS_TSAN = false;
#endif
} // namespace shclog
