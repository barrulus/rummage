#include "rummage/fs/insights.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "rummage/fs/measure.hpp"

namespace rummage::fs {
namespace {
struct CloseDir {
    void operator()(DIR* dir) const { closedir(dir); }
};

using Directory = std::unique_ptr<DIR, CloseDir>;

Directory open_directory(int parent, const char* name, const struct stat& expected) {
    const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return {};
    struct stat actual{};
    if (fstat(fd, &actual) != 0 || actual.st_dev != expected.st_dev ||
        actual.st_ino != expected.st_ino) {
        close(fd);
        return {};
    }
    Directory result{fdopendir(fd)};
    if (!result)
        close(fd);
    return result;
}
}  // namespace

bool InsightSnapshot::complete() const {
    return !limited && !cancelled && !remote && unreadable == 0 && skipped_folders == 0;
}

std::filesystem::path InsightSnapshot::relative_path(std::size_t node) const {
    std::vector<std::string_view> parts;
    while (node > 0 && node < nodes.size()) {
        parts.push_back(nodes[node].name);
        node = nodes[node].parent;
    }
    std::filesystem::path path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        path /= *it;
    return path;
}

std::string insight_extension(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size())
        return {};
    std::string suffix{name.substr(dot)};
    const auto fold = [](char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
    };
    std::ranges::transform(suffix, suffix.begin(), fold);
    if ((suffix == ".gz" || suffix == ".bz2" || suffix == ".xz" || suffix == ".zst") && dot >= 4) {
        std::string before{name.substr(dot - 4, 4)};
        std::ranges::transform(before, before.begin(), fold);
        if (before == ".tar")
            return before + suffix;
    }
    return suffix;
}

InsightSnapshot scan_insights(const std::filesystem::path& root, const InsightOptions& options,
                              const core::CancelToken& token,
                              const std::function<void(InsightProgress)>& progress) {
    InsightSnapshot result;
    auto scan_root = root;
    while (!scan_root.has_filename() && scan_root != scan_root.root_path())
        scan_root = scan_root.parent_path();
    result.root = scan_root;
    result.nodes.push_back({.name = scan_root.filename().string(), .directory = true});
    const auto finish = [&] {
        // Discovery is breadth-first, but consumers use contiguous descendant ranges.
        // Reorder once, without storing full paths or opening one descriptor per folder.
        const auto count = result.nodes.size();
        std::vector<std::size_t> first(count, count), next(count, count);
        for (std::size_t i = 1; i < count; ++i) {
            const auto parent = result.nodes[i].parent;
            next[i] = first[parent];
            first[parent] = i;
        }
        std::vector<InsightNode> ordered;
        ordered.reserve(count);
        std::vector<std::pair<std::size_t, std::size_t>> pending{{0, 0}};
        while (!pending.empty()) {
            const auto [old, parent] = pending.back();
            pending.pop_back();
            const auto id = ordered.size();
            ordered.push_back(std::move(result.nodes[old]));
            ordered.back().parent = parent;
            ordered.back().end = id + 1;
            for (auto child = first[old]; child != count; child = next[child])
                pending.emplace_back(child, id);
        }
        result.nodes = std::move(ordered);
        for (std::size_t i = count; i-- > 1;) {
            const auto& node = result.nodes[i];
            auto& parent = result.nodes[node.parent];
            parent.files += node.files;
            parent.bytes += node.bytes;
            parent.partial = parent.partial || node.partial;
            parent.end = std::max(parent.end, node.end);
        }
        result.nodes[0].partial = !result.complete();
        return std::move(result);
    };
    if (token.cancelled()) {
        result.cancelled = true;
        return finish();
    }
    if (is_remote(scan_root)) {
        result.remote = true;
        return finish();
    }
    struct stat root_info{};
    if (lstat(scan_root.c_str(), &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        ++result.unreadable;
        return finish();
    }
    Directory root_dir = open_directory(AT_FDCWD, scan_root.c_str(), root_info);
    if (!root_dir) {
        ++result.unreadable;
        return finish();
    }

    struct Folder {
        std::size_t node, parent, depth;
        struct stat identity;
    };

    // All immediate children are discovered before any descendants. Pending folders
    // remain explicitly partial if the shared scan budget runs out.
    std::vector<Folder> queue{{0, 0, 0, root_info}};
    std::set<std::pair<dev_t, ino_t>> visited{{root_info.st_dev, root_info.st_ino}};
    const auto started = std::chrono::steady_clock::now();
    auto told = started;
    InsightProgress totals;
    std::size_t names = 0, examined = 0;
    const auto exhausted = [&] {
        if (token.cancelled())
            result.cancelled = true;
        else if (examined >= options.max_entries ||
                 std::chrono::steady_clock::now() - started >= options.time_limit)
            result.limited = true;
        return result.cancelled || result.limited;
    };
    result.nodes[0].partial = true;
    for (std::size_t cursor = 0; cursor < queue.size() && !exhausted(); ++cursor) {
        const auto current = queue[cursor];
        Directory opened;
        DIR* directory = root_dir.get();
        if (cursor != 0) {
            std::vector<std::size_t> ancestors;
            for (auto id = cursor; id != 0; id = queue[id].parent)
                ancestors.push_back(id);
            // Reopen each component relative to the verified root. Never follow a
            // substituted symlink or enter a different inode after discovery.
            for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
                if (exhausted())
                    break;
                const auto& ancestor = queue[*it];
                auto child = open_directory(
                    dirfd(directory), result.nodes[ancestor.node].name.c_str(), ancestor.identity);
                if (!child) {
                    ++result.unreadable;
                    directory = nullptr;
                    break;
                }
                opened = std::move(child);
                directory = opened.get();
            }
            if (!directory || exhausted())
                continue;
        }
        bool failed_entry = false;
        while (!exhausted()) {
            errno = 0;
            const dirent* entry = readdir(directory);
            if (!entry) {
                if (errno != 0)
                    ++result.unreadable;
                else
                    result.nodes[current.node].partial = failed_entry;
                break;
            }
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..")
                continue;
            ++examined;
            if (!options.hidden && name.starts_with('.'))
                continue;
            names += name.size();
            if (names > options.max_name_bytes) {
                result.limited = true;
                break;
            }
            struct stat info{};
            if (fstatat(dirfd(directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
                ++result.unreadable;
                // Retain this failure even after reaching the end of the directory.
                failed_entry = true;
                continue;
            }
            if (S_ISLNK(info.st_mode)) {
                ++result.links;
                continue;
            }
            if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
                continue;
            const auto id = result.nodes.size();
            const bool is_directory = S_ISDIR(info.st_mode);
            const auto bytes =
                !is_directory && info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
            result.nodes.push_back({.name = std::string{name},
                                    .parent = current.node,
                                    .files = is_directory ? 0U : 1U,
                                    .bytes = bytes,
                                    .directory = is_directory,
                                    .partial = is_directory && options.recursive});
            if (!is_directory) {
                ++totals.files;
                totals.bytes += bytes;
            } else if (options.recursive) {
                if (info.st_dev != root_info.st_dev || current.depth + 1 >= options.max_depth ||
                    !visited.emplace(info.st_dev, info.st_ino).second) {
                    ++result.skipped_folders;
                } else {
                    queue.push_back({id, cursor, current.depth + 1, info});
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (progress && now - told >= std::chrono::milliseconds{150}) {
                progress(totals);
                told = now;
            }
        }
    }
    return finish();
}

}  // namespace rummage::fs
