#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rummage::core {

class FileWatcher;
class FileWatchService;

// Options for EventLoop::add_fd.
struct FdOptions {
    // Called on every iteration right before poll(): Wayland's prepare_read and flush.
    std::function<void()> before_poll;
    // Complete prepared I/O before any source callback (including posted work) can run.
    // Called with zero events if polling is interrupted, fails, or preparation quits the loop.
    // Must not dispatch application callbacks; Wayland reads or cancels its prepared read here.
    std::function<void(short revents)> after_poll{};
    // Call the source callback after every poll(), even with revents == 0.
    bool notify_always{false};
};

// The application's single-threaded event loop: one poll() over every file descriptor source
// (Wayland socket, signals, key repeat, timers, inotify, work results).
//
// Everything runs on the thread that calls run(), except post(), which any thread may call to
// queue a function for the loop thread. Sources may be added and removed from inside callbacks.
class EventLoop {
public:
    using SourceId = std::uint64_t;
    using FdCallback = std::function<void(short revents)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // Watches fd (not owned) for events (POLLIN, ...). Returns an id for remove().
    SourceId add_fd(int fd, short events, FdCallback callback, FdOptions options = {});

    // Calls callback after delay, then every interval (zero interval: once, then the timer is
    // removed automatically).
    SourceId add_timer(std::chrono::milliseconds delay, std::chrono::milliseconds interval,
                       std::function<void()> callback);

    // Removes a source; unknown or already removed ids are ignored.
    void remove(SourceId id);

    // Thread-safe: queues fn to run on the loop thread, in posting order.
    void post(std::function<void()> fn);

    // Runs until quit(); returns the code given to quit().
    int run();

    // Ends run() after the current iteration. Loop thread only; other threads post() it.
    void quit(int code = 0);

private:
    friend class FileWatcher;
    struct Source;
    // Weak ownership: the last FileWatcher releases the shared inotify source.
    std::weak_ptr<FileWatchService> file_watch_service_;

    void run_posted();

    std::vector<Source> sources_;
    SourceId next_id_{1};
    bool running_{false};
    int exit_code_{0};

    int wake_fd_{-1};  // eventfd signalled by post()
    std::mutex posted_mutex_;
    std::vector<std::function<void()>> posted_;
};

}  // namespace rummage::core
