#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"

namespace rummage::fs {

// Small, watched probes for the folders visible in a tree. Reads run on workers and stop
// at the first non-hidden subfolder; no file metadata or recursive listings are loaded.
class Subfolders {
public:
    Subfolders(core::WorkerPool& pool, DirectoryWatcher& watcher, std::function<void()> changed);
    ~Subfolders();
    Subfolders(const Subfolders&) = delete;
    Subfolders& operator=(const Subfolders&) = delete;

    void retain(std::span<const std::filesystem::path> paths);
    // Unknown (including unreadable) folders have no disclosure until a probe succeeds.
    [[nodiscard]] std::optional<bool> contains(const std::filesystem::path& path,
                                               bool show_hidden) const;
    [[nodiscard]] bool loading() const;

private:
    struct Result {
        bool visible{false};
        bool hidden{false};
    };

    struct Probe {
        std::optional<Result> result;
        core::CancelToken job;
        DirectoryWatcher::WatchId watch{0};
        bool running{false};
        bool again{false};
    };

    void refresh(const std::filesystem::path& path);
    static std::optional<Result> read(const std::filesystem::path& path,
                                      const core::CancelToken& token);
    core::WorkerPool& pool_;
    DirectoryWatcher& watcher_;
    std::function<void()> changed_;
    std::map<std::filesystem::path, Probe> probes_;
};

}  // namespace rummage::fs
