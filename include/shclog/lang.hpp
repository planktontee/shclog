#pragma once

#include <cstdlib>
#include <expected>
#include <print>
#include <source_location>
#include <type_traits>
#include <utility>
namespace shclog::lang {

template <typename T, typename E>
[[nodiscard]] inline T unwrap(
    std::expected<T, E> opt,
    const std::source_location loc = std::source_location::current()) noexcept {
    if (!opt.has_value()) [[unlikely]] {
        std::println(stderr, "{}:{}: unwrap failed: {}", loc.file_name(),
                     loc.line(), std::to_underlying(opt.error()));
        std::abort();
    }
    return std::move(opt).value();
}

template <typename E>
concept ResultEnum = std::is_enum_v<E> && requires { E::Success; };

template <ResultEnum E>
void unwrap(E e, const std::source_location loc =
                     std::source_location::current()) noexcept {
    if (e != E::Success) [[unlikely]] {
        std::println(stderr, "{}:{}: unwrap failed: {}", loc.file_name(),
                     loc.line(), std::to_underlying(e));
        std::abort();
    }
}

} // namespace shclog::lang
