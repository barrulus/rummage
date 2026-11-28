#include "rummage/fs/subfolders.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>

namespace rummage::fs {

Subfolders::Subfolders(core::WorkerPool& pool, DirectoryWatcher& watcher,
                       std::function<void()> changed)
    : pool_{pool}, watcher_{watcher}, changed_{std::move(changed)} {}

Subfolders::~Subfolders() { retain({}); }

void Subfolders::retain(std::span<const std::filesystem::path> paths) {
    const std::unordered_set<std::filesystem::path> kept{paths.begin(), paths.end()};
    std::erase_if(probes_, [&](auto& item) {
        if (kept.contains(item.first))
            return false;
        item.second.job.cancel();
        watcher_.unwatch(item.second.watch);
        return true;
    });
    for (const auto& path : paths) {
        if (!path.is_absolute() || probes_.contains(path))
            continue;
        auto& probe = probes_[path];
        probe.watch = watcher_.watch_async(
            pool_, path, [this, path](const auto&) { refresh(path); },
            [this, path](std::error_code error) {
                // Initial probing need not wait for a slow registration. Recheck after
                // adoption to cover changes before the loop started receiving events.
                if (!error)
                    refresh(path);
            });
        refresh(path);
    }
}

std::optional<bool> Subfolders::contains(const std::filesystem::path& path,
                                         bool show_hidden) const {
    const auto found = probes_.find(path);
    if (found == probes_.end() || !found->second.result)
        return std::nullopt;
    const auto& result = *found->second.result;
    return result.visible || (show_hidden && result.hidden);
}

bool Subfolders::loading() const {
    return std::ranges::any_of(probes_, [](const auto& item) { return item.second.running; });
}

void Subfolders::refresh(const std::filesystem::path& path) {
    auto& probe = probes_.at(path);
    if (probe.running) {
        probe.again = true;
        return;
    }
    probe.running = true;
    probe.job = pool_.submit(
        core::Priority::nearby,
        [path](const core::CancelToken& token) { return read(path, token); },
        [this, path](std::optional<Result> result) {
            auto& current = probes_.at(path);
            current.running = false;
            if (std::exchange(current.again, false)) {
                refresh(path);
                return;
            }
            current.result = result;
            changed_();
        });
}

std::optional<Subfolders::Result> Subfolders::read(const std::filesystem::path& path,
                                                   const core::CancelToken& token) {
    struct Close {
        void operator()(DIR* dir) const { closedir(dir); }
    };

    const std::unique_ptr<DIR, Close> dir{opendir(path.c_str())};
    if (!dir)
        return std::nullopt;
    Result result;
    for (;;) {
        if (token.cancelled())
            return std::nullopt;
        errno = 0;
        const auto* entry = readdir(dir.get());
        if (!entry)
            return errno == 0 ? std::optional{result} : std::nullopt;
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..")
            continue;
        bool folder = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            struct stat info{};
            folder =
                fstatat(dirfd(dir.get()), entry->d_name, &info, 0) == 0 && S_ISDIR(info.st_mode);
        }
        if (!folder)
            continue;
        if (name.front() == '.')
            result.hidden = true;
        else {
            result.visible = true;
            return result;
        }
    }
}

}  // namespace rummage::fs
