#pragma once

#include "shclog/cast.hpp"
#include "shclog/io/file.hpp"
#include "shclog/types.hpp"
#include <sched.h>

namespace shclog::io::dir {
using namespace shclog::io::file;

struct Dir {
    unique_fd fd;
};
} // namespace shclog::io::dir
