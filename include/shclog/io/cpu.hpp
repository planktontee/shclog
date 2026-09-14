#pragma once

#include "shclog/const.hpp"
#include "shclog/io/file.hpp"
#include "shclog/io/process.hpp"
#include "shclog/io/syscall.hpp"
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <linux/openat2.h>
#include <optional>
#include <print>
#include <ranges>
#include <sys/types.h>
#include <vector>

namespace shclog::io::cpu {

using namespace shclog::io::syscall;
using namespace shclog::io::process;
using namespace shclog::io::file;

enum SetCpuAffinityResult {
    Success,
    InvalidParam,
    Unexpected,
};

SetCpuAffinityResult _set_cpu_affinity(const cpu_set_t &set,
                                       const pthread_t thread_target) noexcept;

SetCpuAffinityResult set_cpu_afinity(
    const size_t cpu,
    const std::optional<const pthread_t> target = std::nullopt) noexcept;

template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, size_t>
SetCpuAffinityResult set_cpu_afinity(
    R &&range,
    const std::optional<const pthread_t> target = std::nullopt) noexcept {

    cpu_set_t set;
    CPU_ZERO(&set);
    for (const size_t cpu : range)
        CPU_SET(cpu, &set);

    return _set_cpu_affinity(set, pthread_t_uwrap(target));
}

enum class ListCpuCoresError {
    Unexpected,
    CouldNotReadCPUFile,
    MalformedFile,
};

struct CPUCore {
  public:
    size_t core_idx;
    std::vector<size_t> siblings;
};

template <auto _STUB = std::to_array("/sys/devices/system/cpu/cpu")>
std::expected<std::vector<CPUCore>, ListCpuCoresError>
list_cpu_cores() noexcept {
    std::vector<CPUCore> result;
    result.reserve(16);

    constexpr auto stub = _STUB;
    constexpr auto path_stub_end =
        std::to_array("/topology/thread_siblings_list\0");
    constexpr size_t cpu_idx_buf_size = 4;

    std::array<uint8_t,
               stub.size() - 1 + path_stub_end.size() - 1 + cpu_idx_buf_size>
        path;
    std::memcpy(path.begin(), stub.begin(), stub.size() - 1);

    std::span<uint8_t, cpu_idx_buf_size> cpu_idx_buf{&path.at(stub.size() - 1),
                                                     cpu_idx_buf_size};

    open_how how{
        .flags = O_RDONLY | O_CLOEXEC,
        .mode = 0,
        .resolve = 0,
    };

    std::array<uint8_t, 64> buf{};

    // 10000 is completely arbitrary
    for (size_t i = 0; i < 10000; ++i) {
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

        auto read_r = pread(cpu_fd.get(), buf, static_cast<int64_t>(0));
        if (!read_r.has_value())
            return std::unexpected(ListCpuCoresError::CouldNotReadCPUFile);

        const uint8_t *content_it = buf.data();
        const uint8_t *const content_end = content_it + read_r.value();

        CPUCore core;
        while (content_it != content_end) {
            auto comma_p = static_cast<const uint8_t *>(
                std::memchr(content_it, ',', content_end - content_it));

            if (!comma_p)
                comma_p = content_end;

            size_t cpu_idx{};
            const auto parse_idx_r = std::from_chars(
                reinterpret_cast<const char *>(content_it),
                reinterpret_cast<const char *>(comma_p), cpu_idx);
            const uint8_t *const parse_idx_end =
                reinterpret_cast<const uint8_t *>(parse_idx_r.ptr);
            if (parse_idx_end == content_it)
                return std::unexpected(ListCpuCoresError::MalformedFile);
            core.siblings.push_back(cpu_idx);

            content_it = parse_idx_end;
            // handle range
            if (content_it != comma_p && *content_it == '-') {
                ++content_it;

                size_t cpu_idx_end{};
                const auto parse_idx_end_r = std::from_chars(
                    reinterpret_cast<const char *>(content_it),
                    reinterpret_cast<const char *>(comma_p), cpu_idx_end);
                const uint8_t *parse_idx_end_end =
                    reinterpret_cast<const uint8_t *>(parse_idx_end_r.ptr);
                if (parse_idx_end_end == content_it)
                    return std::unexpected(ListCpuCoresError::MalformedFile);
                for (++cpu_idx; cpu_idx <= cpu_idx_end; ++cpu_idx)
                    core.siblings.push_back(cpu_idx);

                content_it = parse_idx_end_end;
            }
            ++content_it;
        }
        if constexpr (IS_DEBUG) {
            if (*(content_end - 1) != '\n')
                return std::unexpected(ListCpuCoresError::MalformedFile);
        }

        core.core_idx = i;
        result.push_back(core);
    }

    return result;
}

} // namespace shclog::io::cpu
