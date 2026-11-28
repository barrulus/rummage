#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"

namespace rummage::fs {

struct DirectoryEvent {
    enum class Kind : std::uint8_t {
        created,         // a name appeared (new file, link, directory)
        deleted,         // a name went away
        moved_from,      // renamed away; a moved_to with the same cookie may follow
        moved_to,        // renamed in; cookie pairs it with its moved_from
        changed,         // content written and closed, or metadata changed
        directory_gone,  // the watched directory was deleted, moved or unmounted
        overflow,        // the kernel queue overflowed: events were lost, re-read everything
    };

    Kind kind;
    std::string name;  // inside the watched directory; empty for directory_gone and overflow
    std::uint32_t cookie{0};
};

// Watches directories through one inotify instance shared by all of them (the kernel limits
// instances per user, and Miller columns open several directories). Events reach each
// directory's handler on the loop thread, in kernel order, one batch per read.
//
// Must not outlive the loop. Not copyable or movable: the loop callback keeps a pointer to it.
class DirectoryWatcher {
public:
    using Handler = std::function<void(const std::vector<DirectoryEvent>& events)>;
    using WatchId = std::uint64_t;

    // Optional registrar is useful for embedding and deterministic slow-filesystem tests.
    using Registrar = std::function<int(int, const char*, std::uint32_t)>;
    explicit DirectoryWatcher(core::EventLoop& loop, Registrar registrar = {});
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
    DirectoryWatcher(DirectoryWatcher&&) = delete;
    DirectoryWatcher& operator=(DirectoryWatcher&&) = delete;

    // Starts watching a directory; 0 and error set on failure (ENOENT, EACCES, ENOSPC when the
    // per-user watch limit is reached...). The same directory may be watched more than once.
    WatchId watch(const std::filesystem::path& directory, Handler handler, std::error_code& error);

    // Resolves/registers paths on a worker, using one additional shared inotify instance.
    // Returns a subscription immediately; completion and events run on the loop thread.
    // unwatch cancels pending registration too. No callback runs after unwatch/destruction.
    // Returns 0, without a callback, when 8192 active/pending subscriptions are retained;
    // callers may load without live updates. A dropped pool task releases its reservation.
    WatchId watch_async(core::WorkerPool& pool, const std::filesystem::path& directory,
                        Handler handler, std::function<void(std::error_code)> completion = {});

    // Stops one watch; unknown ids are ignored. Safe from inside a handler.
    void unwatch(WatchId id);

private:
    struct Subscriber {
        WatchId id;
        Handler handler;
    };

    struct AsyncState;
    struct AsyncRegistration;
    void on_readable(bool asynchronous = false);
    void cleanup_async();

    core::EventLoop& loop_;
    int fd_{-1};
    core::EventLoop::SourceId source_{0};
    WatchId next_id_{1};
    Registrar registrar_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::shared_ptr<AsyncState> async_;
    core::EventLoop::SourceId async_source_{0}, cleanup_timer_{0};
    std::unordered_map<WatchId, std::shared_ptr<AsyncRegistration>> async_registrations_;
    std::unordered_map<int, std::vector<Subscriber>> async_by_descriptor_;
    std::unordered_map<WatchId, int> async_descriptor_of_;
    // Kernel watch descriptor -> its subscribers (inotify returns the same descriptor when one
    // directory is watched twice).
    std::unordered_map<int, std::vector<Subscriber>> by_descriptor_;
    std::unordered_map<WatchId, int> descriptor_of_;
};

}  // namespace rummage::fs
