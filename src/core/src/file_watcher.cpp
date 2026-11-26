#include "rummage/core/file_watcher.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/inotify.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rummage/core/log.hpp"

namespace rummage::core {
namespace {

// Writes closed, renames into and out of the directory, deletions. IN_MODIFY is left out on
// purpose: it fires on every write() and after a truncation, before the content is complete.
constexpr std::uint32_t kEvents =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE;

}  // namespace

// Owned by subscribers; the loop only keeps a weak reference. DirectoryWatcher uses its own
// descriptor, so its masks and directory-model event handling cannot be changed by this service.
class FileWatchService {
public:
    explicit FileWatchService(EventLoop& loop) : loop_{loop} {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0)
            throw std::system_error{errno, std::generic_category(), "inotify_init1"};
        try {
            source_ = loop_.add_fd(fd_, POLLIN, [this](short) { on_readable(); });
        } catch (...) {
            close(fd_);
            throw;
        }
    }

    ~FileWatchService() {
        loop_.remove(source_);
        close(fd_);
    }

    int subscribe(const std::filesystem::path& directory, FileWatcher& watcher) {
        const int wd =
            inotify_add_watch(fd_, directory.c_str(), kEvents | IN_MASK_ADD | IN_ONLYDIR);
        if (wd < 0)
            throw std::system_error{errno, std::generic_category(),
                                    "inotify_add_watch " + directory.string()};
        try {
            watchers_[wd].push_back(&watcher);
        } catch (...) {
            const auto it = watchers_.find(wd);
            if (it == watchers_.end() || it->second.empty()) {
                watchers_.erase(wd);
                inotify_rm_watch(fd_, wd);
            }
            throw;
        }
        return wd;
    }

    void unsubscribe(int wd, FileWatcher& watcher) {
        const auto it = watchers_.find(wd);
        if (it == watchers_.end())
            return;
        std::erase(it->second, &watcher);
        if (it->second.empty()) {
            inotify_rm_watch(fd_, wd);
            watchers_.erase(it);
        }
    }

private:
    void on_readable() {
        alignas(inotify_event) std::array<char, 4096> buffer{};
        std::unordered_set<FileWatcher*> changed;
        for (;;) {
            const ssize_t length = read(fd_, buffer.data(), buffer.size());
            if (length < 0 && errno == EINTR)
                continue;
            if (length <= 0) {
                if (length < 0 && errno != EAGAIN)
                    log::warn("file watcher: read failed: {}", std::strerror(errno));
                break;
            }
            for (std::size_t offset = 0; std::cmp_less(offset, length);) {
                inotify_event event{};
                std::memcpy(&event, buffer.data() + offset, sizeof event);
                const char* name = buffer.data() + offset + sizeof event;
                if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                    // Every subscription may have lost events, including other directories.
                    for (const auto& [wd, watchers] : watchers_) {
                        (void)wd;
                        for (auto* watcher : watchers)
                            changed.insert(watcher);
                    }
                } else if (const auto it = watchers_.find(event.wd); it != watchers_.end()) {
                    for (auto* watcher : it->second) {
                        if (event.len > 0 && watcher->name_ == name)
                            changed.insert(watcher);
                        if ((event.mask & IN_IGNORED) != 0U) {
                            watcher->watch_ = -1;
                            log::warn(
                                "file watcher: directory of {} gone, changes no longer "
                                "followed",
                                watcher->name_);
                        }
                    }
                    if ((event.mask & IN_IGNORED) != 0U)
                        watchers_.erase(it);
                }
                offset += sizeof event + event.len;
            }
        }
        // Coalesce a whole read batch before touching timers, including overflow fanout.
        for (auto* watcher : changed)
            watcher->changed();
    }

    EventLoop& loop_;
    int fd_{-1};
    EventLoop::SourceId source_{0};
    std::unordered_map<int, std::vector<FileWatcher*>> watchers_;
};

FileWatcher::FileWatcher(EventLoop& loop, const std::filesystem::path& file, Handler handler,
                         std::chrono::milliseconds settle)
    : loop_{loop}, name_{file.filename().string()}, handler_{std::move(handler)}, settle_{settle} {
    service_ = loop.file_watch_service_.lock();
    if (!service_) {
        service_ = std::make_shared<FileWatchService>(loop);
        loop.file_watch_service_ = service_;
    }
    const auto directory = file.has_parent_path() ? file.parent_path() : std::filesystem::path{"."};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    watch_ = service_->subscribe(directory, *this);
}

FileWatcher::~FileWatcher() {
    loop_.remove(timer_);
    service_->unsubscribe(watch_, *this);
}

void FileWatcher::changed() {
    // Restart the settling interval, so separate read batches still wait for the last write.
    loop_.remove(timer_);
    timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
        timer_ = 0;
        handler_();
    });
}

}  // namespace rummage::core
