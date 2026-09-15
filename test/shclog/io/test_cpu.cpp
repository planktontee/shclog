
#include <array>
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include "doctest.h"
#include <sstream>
#include <sys/mman.h>

#include "shclog/io/cpu.hpp"

using namespace shclog::io::cpu;

TEST_CASE("Parse CPUCore files") {
    auto cores_r = list_cpu_cores<std::to_array("data/cpu_list_data/cpu")>();
    if (!cores_r.has_value()) {
        std::ostringstream err;
        err << "Error: " << static_cast<int>(cores_r.error()) << "\n";
        MESSAGE(err.str());
    }
    CHECK(cores_r.has_value());
    auto cores = cores_r.value();
    CHECK(cores.size() == 3);

    auto expected = std::vector<std::vector<size_t>>{
        std::vector<size_t>{0, 8},
        std::vector<size_t>{1, 3, 4, 5, 6, 7, 9},
        std::vector<size_t>{2, 10},
    };
    for (size_t i = 0; i < cores.size(); ++i) {
        const auto core = cores[i];
        CHECK(core.core_idx == i);
        CHECK(expected[i] == core.siblings);
    }
}
