#pragma once

#include "shclog/cast.hpp"
#include "shclog/const.hpp"
#include "shclog/io/file.hpp"
#include "shclog/io/process.hpp"
#include "shclog/types.hpp"
#include <charconv>
#include <concepts>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <linux/openat2.h>
#include <optional>
#include <print>
#include <ranges>
#include <source_location>
#include <sys/types.h>
#include <vector>

namespace shclog::io::cpu {

using namespace shclog::io::process;
using namespace shclog::io::file;

enum SetCpuAffinityResult : u8 {
    Success,
    InvalidParam,
    Unexpected,
};

SetCpuAffinityResult _set_cpu_affinity(const cpu_set_t &set,
                                       const pthread_t thread_target) noexcept;

SetCpuAffinityResult set_cpu_afinity(
    const usize cpu,
    const std::optional<const pthread_t> target = std::nullopt) noexcept;

template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, usize>
SetCpuAffinityResult set_cpu_afinity(
    R &&range,
    const std::optional<const pthread_t> target = std::nullopt) noexcept {

    cpu_set_t set;
    CPU_ZERO(&set);
    for (const usize cpu : range)
        CPU_SET(cpu, &set);

    return _set_cpu_affinity(set, pthread_t_uwrap(target));
}

enum class ListCpuCoresError : u8 {
    Unexpected,
    CouldNotReadCPUFile,
    MalformedFile,
};

struct CPUCore {
  public:
    usize core_idx = 0;
    std::vector<usize> siblings;

    CPUCore() { siblings.reserve(2); }
};

template <auto PATH_STUB = std::to_array("/sys/devices/system/cpu/cpu")>
std::expected<std::vector<CPUCore>, ListCpuCoresError>
list_cpu_cores() noexcept {
    std::vector<CPUCore> result;
    result.reserve(32);

    constexpr auto stub = PATH_STUB;
    constexpr auto path_stub_end =
        std::to_array("/topology/thread_siblings_list");
    constexpr usize cpu_idx_buf_size = 4;

    std::array<u8, stub.size() - 1 + path_stub_end.size() + cpu_idx_buf_size>
        path{};
    std::memcpy(path.begin(), stub.begin(), stub.size() - 1);

    std::span<u8, cpu_idx_buf_size> cpu_idx_buf{&path.at(stub.size() - 1),
                                                cpu_idx_buf_size};

    open_how how{
        .flags = O_RDONLY | O_CLOEXEC,
        .mode = 0,
        .resolve = 0,
    };

    std::array<u8, 64> buf{};

    // 10000 is completely arbitrary
    for (usize i = 0; i < 10000; ++i) {
        const auto [path_cpu_idx_end, fmt_err] = std::to_chars(
            reinterpret_cast<char *>(cpu_idx_buf.data()),
            reinterpret_cast<char *>(cpu_idx_buf.data() + cpu_idx_buf.size()),
            i);

        if (fmt_err != std::errc()) {
            if constexpr (IS_DEBUG) {
                std::println(stderr, "Panic in {}: {}",
                             std::source_location::current().function_name(),
                             std::make_error_code(fmt_err).message());
                std::abort();
            }
            std::unreachable();
        }
        memcpy(path_cpu_idx_end, path_stub_end.data(), path_stub_end.size());

        const auto open_r = open(reinterpret_cast<char *>(path.data()), &how);
        // this is better than querying getdents64 lol
        if (!open_r.has_value())
            break;
        const auto cpu_fd = unique_fd(open_r.value());

        auto read_r = pread(cpu_fd.get(), buf, static_cast<i64>(0));
        if (!read_r.has_value())
            return std::unexpected(ListCpuCoresError::CouldNotReadCPUFile);

        const u8 *content_it = buf.data();
        const u8 *const content_end = content_it + read_r.value();

        CPUCore core{};
        while (content_it != content_end) {
            auto comma_p = static_cast<const u8 *>(std::memchr(
                content_it, ',', int_cast(content_end - content_it)));

            if (!comma_p)
                comma_p = content_end;

            usize cpu_idx{};
            const auto parse_idx_r = std::from_chars(
                reinterpret_cast<const char *>(content_it),
                reinterpret_cast<const char *>(comma_p), cpu_idx);
            const u8 *const parse_idx_end =
                reinterpret_cast<const u8 *>(parse_idx_r.ptr);
            if (parse_idx_end == content_it)
                return std::unexpected(ListCpuCoresError::MalformedFile);
            core.siblings.push_back(cpu_idx);

            content_it = parse_idx_end;
            // handle range
            if (content_it != comma_p && *content_it == '-') {
                ++content_it;

                usize cpu_idx_end{};
                const auto parse_idx_end_r = std::from_chars(
                    reinterpret_cast<const char *>(content_it),
                    reinterpret_cast<const char *>(comma_p), cpu_idx_end);
                const u8 *parse_idx_end_end =
                    reinterpret_cast<const u8 *>(parse_idx_end_r.ptr);
                if (parse_idx_end_end == content_it)
                    return std::unexpected(ListCpuCoresError::MalformedFile);
                for (++cpu_idx; cpu_idx <= cpu_idx_end; ++cpu_idx)
                    core.siblings.push_back(cpu_idx);

                content_it = parse_idx_end_end;
            }
            ++content_it;
        }
        if (*(content_end - 1) != '\n')
            return std::unexpected(ListCpuCoresError::MalformedFile);

        core.core_idx = i;
        result.push_back(std::move(core));
    }

    return result;
}

} // namespace shclog::io::cpu
