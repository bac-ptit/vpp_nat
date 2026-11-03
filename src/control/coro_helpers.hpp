#pragma once

#include <coroutine>

namespace nat::control {

/**
 * @brief Zero-overhead detached coroutine task for fire-and-forget async operations.
 */
struct DetachedTask {
  struct promise_type {
    DetachedTask get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };
};

/**
 * @brief Cooperative yield point.
 *
 * NOTE: this awaiter currently resumes the coroutine inline, so it is a
 * scheduling no-op. It is kept as an explicit yield point for a future
 * event-loop-backed scheduler; it does not block the calling thread.
 */
struct AsyncYieldAwaiter {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }
  void await_resume() const noexcept {}
};

}  // namespace nat::control
