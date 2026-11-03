#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <utility>

#if __has_include(<print>)
#include <print>
#endif

namespace nat::io {

#if __has_include(<print>)
// GCC 14+ / Clang 18+: use the real C++23 <print> facilities.
using std::print;
using std::println;
#else
namespace detail {

inline std::mutex& LogMutex() {
  static std::mutex mutex;
  return mutex;
}

template <typename... Args>
inline void Write(std::FILE* stream, bool newline, std::format_string<Args...> fmt,
                  Args&&... args) {
  const auto text{std::format(fmt, std::forward<Args>(args)...)};
  const std::lock_guard lock{LogMutex()};
  (void)std::fwrite(text.data(), 1, text.size(), stream);
  if (newline) {
    (void)std::fputc('\n', stream);
  }
}

}  // namespace detail

// C++23 <print> is unavailable on GCC 13; provide equivalent overloads in a
// project namespace instead of adding declarations to namespace std (UB).
template <typename... Args>
inline void print(std::format_string<Args...> fmt, Args&&... args) {
  detail::Write(stdout, false, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void print(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
  detail::Write(stream, false, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void println(std::format_string<Args...> fmt, Args&&... args) {
  detail::Write(stdout, true, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void println(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
  detail::Write(stream, true, fmt, std::forward<Args>(args)...);
}
#endif

}  // namespace nat::io
