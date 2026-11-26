#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/photos/client.hpp"

namespace rummage::photos {

// Catalog-derived subscriptions use the browser's existing inotify authority. Construct
// only after Photo Library has been activated; catalog requests and directory classification
// run on workers. The loop registers at most 16 watches per turn and holds at most 4096.
class SourceWatcher {
public:
    // Empty error: reconciliation jobs were queued; the owner should refresh and drain work.
    using Changed = std::function<void(const std::string& error)>;
    SourceWatcher(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                  std::shared_ptr<Client> client, Changed changed);
    ~SourceWatcher();
    SourceWatcher(const SourceWatcher&) = delete;
    SourceWatcher& operator=(const SourceWatcher&) = delete;

    void refresh_subscriptions();
    void stop();

    // Also accepts replayed/overflow notifications from an embedding owner. It does not
    // read the source tree or perform file operations; it coalesces a catalog refresh job.
    void notify_change(std::int64_t source_id, fs::DirectoryEvent::Kind kind);

    [[nodiscard]] std::size_t subscription_count() const { return registrations_.size(); }

    [[nodiscard]] bool refreshing() const { return listing_; }

private:
    using Id = std::int64_t;

    struct Path {
        Id source;
        std::filesystem::path path;
    };

    struct Registration {
        fs::DirectoryWatcher::WatchId watch;
        Id source;
        std::uint64_t generation;
    };

    struct Candidate {
        Id source;
        std::filesystem::path path;
    };

    void fetch_page(Id after);
    void register_batch();
    void finish_listing(bool success);
    void receive(Id source, const std::filesystem::path& directory,
                 const std::vector<fs::DirectoryEvent>& events);
    void schedule_flush();
    void flush();
    [[nodiscard]] bool excluded(const std::filesystem::path& path) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::shared_ptr<Client> client_;
    Changed changed_;
    std::map<std::filesystem::path, Registration> registrations_;
    std::map<Id, std::filesystem::path> roots_;
    std::map<Id, std::filesystem::path> known_roots_;
    std::set<Id> online_;
    std::vector<std::filesystem::path> exclusions_;
    std::set<std::string> extensions_;
    std::deque<Path> pending_paths_;
    std::set<Id> pending_sources_;
    std::vector<Candidate> candidates_;
    core::CancelToken page_job_, refresh_job_;
    core::EventLoop::SourceId register_timer_{0}, debounce_timer_{0}, availability_timer_{0};
    std::uint64_t generation_{0};
    Id next_page_{0};
    bool more_{false}, listing_{false}, rerun_{false}, flushing_{false}, stopped_{false};
    bool limit_reported_{false};
    std::string registration_error_;
};

}  // namespace rummage::photos
