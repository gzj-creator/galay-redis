#pragma once
// Auto prelude for transitional C++23 module builds on Clang/GCC/MSVC.
// Keep third-party/system/dependency headers in global module fragment.

#if __has_include(<any>)
#include <any>
#endif
#if __has_include(<array>)
#include <array>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<concepts>)
#include <concepts>
#endif
#if __has_include(<condition_variable>)
#include <condition_variable>
#endif
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include(<cstddef>)
#include <cstddef>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<galay-kernel/async/tcp_socket.h>)
#include <galay-kernel/async/tcp_socket.h>
#endif
#if __has_include(<galay-kernel/common/buffer.h>)
#include <galay-kernel/common/buffer.h>
#endif
#if __has_include(<galay-kernel/common/error.h>)
#include <galay-kernel/common/error.h>
#endif
#if __has_include(<galay-kernel/common/host.hpp>)
#include <galay-kernel/common/host.hpp>
#endif
#if __has_include(<galay-kernel/concurrency/async_waiter.h>)
#include <galay-kernel/concurrency/async_waiter.h>
#endif
#if __has_include(<galay-kernel/kernel/awaitable.h>)
#include <galay-kernel/kernel/awaitable.h>
#endif
#if __has_include(<galay-kernel/kernel/io_scheduler.hpp>)
#include <galay-kernel/kernel/io_scheduler.hpp>
#endif
#if __has_include(<galay-kernel/kernel/task.h>)
#include <galay-kernel/kernel/task.h>
#endif
#if __has_include(<galay-kernel/kernel/timeout.hpp>)
#include <galay-kernel/kernel/timeout.hpp>
#endif
#if __has_include(<map>)
#include <map>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<mutex>)
#include <mutex>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<queue>)
#include <queue>
#endif
#if __has_include(<spdlog/sinks/stdout_color_sinks.h>)
#include <spdlog/sinks/stdout_color_sinks.h>
#endif
#if __has_include(<spdlog/spdlog.h>)
#include <spdlog/spdlog.h>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<type_traits>)
#include <type_traits>
#endif
#if __has_include(<unordered_map>)
#include <unordered_map>
#endif
#if __has_include(<utility>)
#include <utility>
#endif
#if __has_include(<variant>)
#include <variant>
#endif
#if __has_include(<vector>)
#include <vector>
#endif
#if __has_include(<galay-ssl/async/ssl_awaitable_core.h>)
#include <galay-ssl/async/ssl_awaitable_core.h>
#endif
#if __has_include(<galay-ssl/async/ssl_socket.h>)
#include <galay-ssl/async/ssl_socket.h>
#endif
#if __has_include(<galay-ssl/ssl/ssl_context.h>)
#include <galay-ssl/ssl/ssl_context.h>
#endif
