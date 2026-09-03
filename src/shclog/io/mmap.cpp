#include "shclog/io/syscall.hpp"
#include <sys/mman.h>

namespace shclog::io::mmap {
using namespace shclog::io::syscall;

void munmap(void *const ptr, const size_t size) noexcept {
    const int rc = ::munmap(ptr, size);
    switch (e_errno(rc)) {
    case Errno::SUCCESS:
        break;
    default:
        debug_e_errno();
        break;
    }
}
} // namespace shclog::io::mmap
