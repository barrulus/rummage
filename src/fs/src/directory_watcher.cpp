#include "rummage/fs/directory_watcher.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::fs {
namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                     IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                     IN_ONLYDIR | IN_EXCL_UNLINK;

}  // namespace

struct DirectoryWatcher::AsyncState {
    int fd{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    std::error_code open_error{fd < 0 ? errno : 0, std::generic_category()};
    // The mutex covers reference bookkeeping only, never pathname lookup. Removal waits
    // until all adds finish, since concurrent inotify_add_watch calls may share a descriptor.
    // The event loop never waits for a worker, even when a mount is unavailable.
    std::atomic<std::size_t> adding{0};
    std::mutex mutex;
    std::unordered_map<int, std::size_t> references;
    std::vector<WatchId> retired;  // loop thread only

    ~AsyncState() {
        if (fd >= 0)
            ::close(fd);
    }
};

struct DirectoryWatcher::AsyncRegistration {
    std::atomic<bool> ready{false};
    int descriptor{-1};
    std::error_code error;
    std::atomic<bool> cancelled{false};
};

DirectoryWatcher::DirectoryWatcher(core::EventLoop& loop, Registrar registrar)
    : loop_{loop},
      fd_{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)},
      registrar_{registrar ? std::move(registrar) : Registrar{::inotify_add_watch}} {
    if (fd_ < 0) {
        throw std::system_error{errno, std::generic_category(), "inotify_init1"};
    }
    source_ = loop_.add_fd(fd_, POLLIN, [this](short /*revents*/) { on_readable(); });
}

DirectoryWatcher::~DirectoryWatcher() {
    *alive_ = false;
    loop_.remove(source_);
    loop_.remove(async_source_);
    loop_.remove(cleanup_timer_);
    // In-flight workers retain the async fd, but no pointer to this object. Closing it only
    // after the last registration finishes prevents fd reuse while the kernel is using it.
    close(fd_);  // drops every watch
}

DirectoryWatcher::WatchId DirectoryWatcher::watch(const std::filesystem::path& directory,
                                                  Handler handler, std::error_code& error) {
    const int descriptor = registrar_(fd_, directory.c_str(), kWatchMask);
    if (descriptor < 0) {
        error = {errno, std::generic_category()};
        return 0;
    }
    error.clear();
    const WatchId id = next_id_++;
    by_descriptor_[descriptor].push_back({id, std::move(handler)});
    descriptor_of_[id] = descriptor;
    return id;
}

DirectoryWatcher::WatchId DirectoryWatcher::watch_async(
    core::WorkerPool& pool, const std::filesystem::path& directory, Handler handler,
    std::function<void(std::error_code)> completion) {
    // Includes retired watches awaiting an outstanding kernel lookup. A stalled mount must
    // not turn rapid navigation into unbounded retained kernel watches or queued work.
    if (async_registrations_.size() >= 8192)
        return 0;
    if (!async_) {
        async_ = std::make_shared<AsyncState>();
        if (async_->fd >= 0)
            async_source_ = loop_.add_fd(async_->fd, POLLIN, [this](short) { on_readable(true); });
    }
    const WatchId id = next_id_++;
    auto registration = std::make_shared<AsyncRegistration>();
    async_registrations_.emplace(id, registration);
    ++async_->adding;
    // The lease is also destroyed when the pool drops a queued task. Its cleanup post uses
    // only a loop lifetime guaranteed by WorkerPool and an independent watcher lifetime flag.
    auto lease = std::shared_ptr<void>(nullptr, [state = async_, registration, alive = alive_,
                                                 loop = &loop_, self = this, id](void*) {
        const bool dropped = !registration->ready.load(std::memory_order_acquire);
        if (dropped) {
            registration->error = std::make_error_code(std::errc::operation_canceled);
            registration->ready.store(true, std::memory_order_release);
        }
        --state->adding;
        loop->post([alive, self, registration, dropped, id] {
            if (!*alive)
                return;
            if (dropped && !registration->cancelled.exchange(true))
                self->async_->retired.push_back(id);
            self->cleanup_async();
        });
    });
    pool.submit(
        core::Priority::nearby,
        [state = async_, registration, directory, registrar = registrar_,
         lease](const core::CancelToken&) {
            if (registration->cancelled.load())
                registration->error = std::make_error_code(std::errc::operation_canceled);
            else if (state->open_error)
                registration->error = state->open_error;
            else {
                registration->descriptor = registrar(state->fd, directory.c_str(), kWatchMask);
                if (registration->descriptor < 0)
                    registration->error = {errno, std::generic_category()};
            }
            if (registration->descriptor >= 0) {
                const std::lock_guard lock{state->mutex};
                ++state->references[registration->descriptor];
            }
            registration->ready.store(true, std::memory_order_release);
            return registration;
        },
        [this, alive = alive_, id, handler = std::move(handler),
         completion = std::move(completion)](const auto& registered) mutable {
            if (!*alive)
                return;
            if (registered->cancelled) {
                cleanup_async();
                return;
            }
            const auto error = registered->error;
            if (!error) {
                async_by_descriptor_[registered->descriptor].push_back({id, std::move(handler)});
                async_descriptor_of_[id] = registered->descriptor;
            } else {
                registered->cancelled = true;
                async_->retired.push_back(id);
                cleanup_async();
            }
            if (completion)
                completion(error);
        });
    return id;
}

void DirectoryWatcher::cleanup_async() {
    if (!async_ || async_->adding.load() != 0 || async_->retired.empty())
        return;  // the last task completion/destruction retries; never poll a blocked mount
    std::unique_lock lock{async_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        if (cleanup_timer_ == 0)
            cleanup_timer_ = loop_.add_timer(std::chrono::milliseconds{10},
                                             std::chrono::milliseconds{0}, [this] {
                                                 cleanup_timer_ = 0;
                                                 cleanup_async();
                                             });
        return;
    }
    for (const auto id : async_->retired) {
        const auto found = async_registrations_.find(id);
        if (found == async_registrations_.end())
            continue;
        const auto& registration = found->second;
        if (registration->descriptor >= 0) {
            const auto reference = async_->references.find(registration->descriptor);
            if (reference != async_->references.end() && --reference->second == 0) {
                inotify_rm_watch(async_->fd, registration->descriptor);
                async_->references.erase(reference);
            }
        }
        async_registrations_.erase(found);
    }
    async_->retired.clear();
}

void DirectoryWatcher::unwatch(WatchId id) {
    if (const auto pending = async_registrations_.find(id); pending != async_registrations_.end()) {
        if (pending->second->cancelled.exchange(true))
            return;
        async_->retired.push_back(id);
        if (const auto active = async_descriptor_of_.find(id);
            active != async_descriptor_of_.end()) {
            const int descriptor = active->second;
            async_descriptor_of_.erase(active);
            auto& subscribers = async_by_descriptor_[descriptor];
            std::erase_if(subscribers,
                          [id](const Subscriber& subscriber) { return subscriber.id == id; });
            if (subscribers.empty())
                async_by_descriptor_.erase(descriptor);
        }
        cleanup_async();
        return;
    }
    const auto it = descriptor_of_.find(id);
    if (it == descriptor_of_.end()) {
        return;
    }
    const int descriptor = it->second;
    descriptor_of_.erase(it);
    auto& subscribers = by_descriptor_[descriptor];
    std::erase_if(subscribers, [id](const Subscriber& s) { return s.id == id; });
    if (subscribers.empty()) {
        by_descriptor_.erase(descriptor);
        inotify_rm_watch(fd_, descriptor);  // may fail if the kernel dropped it already: fine
    }
}

void DirectoryWatcher::on_readable(bool asynchronous) {
    const int fd = asynchronous ? async_->fd : fd_;
    auto& by_descriptor = asynchronous ? async_by_descriptor_ : by_descriptor_;
    auto& descriptor_of = asynchronous ? async_descriptor_of_ : descriptor_of_;
    std::unordered_map<int, std::vector<DirectoryEvent>> batches;
    std::vector<int> order;  // descriptors in first-event order
    bool overflow = false;
    alignas(inotify_event) std::array<char, 16384> buffer{};
    for (;;) {
        const ssize_t length = read(fd, buffer.data(), buffer.size());
        if (length <= 0) {
            if (length < 0 && errno != EAGAIN && errno != EINTR) {
                log::warn("directory watcher: read failed: {}",
                          std::error_code{errno, std::generic_category()}.message());
            }
            break;
        }
        for (std::size_t offset = 0; std::cmp_less(offset, length);) {
            inotify_event event{};
            std::memcpy(&event, buffer.data() + offset, sizeof event);
            const char* name_bytes = buffer.data() + offset + sizeof event;
            offset += sizeof event + event.len;

            if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                overflow = true;
                continue;
            }
            using Kind = DirectoryEvent::Kind;
            Kind kind{};
            if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0U) {
                kind = Kind::directory_gone;
            } else if ((event.mask & IN_CREATE) != 0U) {
                kind = Kind::created;
            } else if ((event.mask & IN_DELETE) != 0U) {
                kind = Kind::deleted;
            } else if ((event.mask & IN_MOVED_FROM) != 0U) {
                kind = Kind::moved_from;
            } else if ((event.mask & IN_MOVED_TO) != 0U) {
                kind = Kind::moved_to;
            } else if ((event.mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0U) {
                kind = Kind::changed;
            } else {
                continue;  // IN_IGNORED and anything else
            }
            if (!by_descriptor.contains(event.wd)) {
                continue;  // unwatched meanwhile
            }
            if (!batches.contains(event.wd)) {
                order.push_back(event.wd);
            }
            batches[event.wd].push_back(
                {.kind = kind,
                 .name = event.len > 0 ? std::string{name_bytes} : std::string{},
                 .cookie = event.cookie});
        }
    }
    if (overflow) {
        log::warn("directory watcher: inotify queue overflow, directories will be re-read");
        for (const auto& [descriptor, subscribers] : by_descriptor) {
            if (!batches.contains(descriptor)) {
                order.push_back(descriptor);
            }
            batches[descriptor].push_back({.kind = DirectoryEvent::Kind::overflow, .name = {}});
        }
    }
    for (const int descriptor : order) {
        // Copy: a handler may unwatch (itself or others) while we iterate.
        const auto it = by_descriptor.find(descriptor);
        if (it == by_descriptor.end()) {
            continue;
        }
        const std::vector<Subscriber> subscribers = it->second;
        for (const Subscriber& subscriber : subscribers) {
            if (descriptor_of.contains(subscriber.id)) {
                subscriber.handler(batches[descriptor]);
            }
        }
    }
}

}  // namespace rummage::fs
