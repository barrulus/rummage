#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

#include "rummage/core/event_loop.hpp"

namespace rummage::app {

// Owns a window's suspended scope. Its controls and the locals captured by their handlers
// remain alive until closing resumes the scope; destruction unwinds it on application exit.
// The application reaps completed sessions after the event handler has returned.
class WindowSession {
public:
    struct promise_type {
        std::exception_ptr error;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);

        WindowSession get_return_object() {
            return WindowSession{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    explicit WindowSession(std::coroutine_handle<promise_type> handle) : handle_{handle} {}

    WindowSession(WindowSession&& other) noexcept : handle_{std::exchange(other.handle_, {})} {}

    WindowSession& operator=(WindowSession&& other) noexcept {
        if (this != &other) {
            release();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    WindowSession(const WindowSession&) = delete;
    WindowSession& operator=(const WindowSession&) = delete;

    ~WindowSession() { release(); }

    [[nodiscard]] bool done() const { return !handle_ || handle_.done(); }

    void rethrow() const {
        if (handle_ && handle_.promise().error)
            std::rethrow_exception(handle_.promise().error);
    }

private:
    void release() {
        if (handle_) {
            *handle_.promise().alive = false;
            handle_.destroy();
        }
    }

    std::coroutine_handle<promise_type> handle_;
};

struct WindowLifetime {
    bool requested{};
    std::coroutine_handle<> continuation;
    std::weak_ptr<bool> alive;

    bool await_ready() const noexcept { return requested; }

    void await_suspend(std::coroutine_handle<WindowSession::promise_type> handle) noexcept {
        continuation = handle;
        alive = handle.promise().alive;
    }

    void await_resume() const noexcept {}

    // Never unwind a window while one of its draw/input callbacks is still using it.
    void close(core::EventLoop& loop, std::function<void()> finished) {
        if (std::exchange(requested, true))
            return;
        if (auto handle = std::exchange(continuation, {}))
            loop.post([handle, valid = alive, finished = std::move(finished)] {
                if (const auto living = valid.lock(); living && *living) {
                    handle.resume();
                    finished();
                }
            });
    }
};

}  // namespace rummage::app
