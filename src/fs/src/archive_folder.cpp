#include "rummage/fs/archive_folder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>

namespace rummage::fs {

namespace {

constexpr std::size_t kMaxName = 255;
constexpr std::size_t kKeptTrees = 4;

// The folder holding path ("" for the top), and its last part.
std::pair<std::string, std::string> split_last(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

constexpr std::int64_t kBillion = 1'000'000'000;
constexpr std::int64_t kMostSeconds = std::numeric_limits<std::int64_t>::max() / kBillion;

std::int64_t nanoseconds(std::int64_t seconds) {
    return std::clamp(seconds, -kMostSeconds, kMostSeconds) * kBillion;
}

}  // namespace

std::optional<ArchiveLocation> find_archive(const std::filesystem::path& path) {
    std::filesystem::path prefix;
    std::filesystem::path rest;
    bool found = false;
    for (const std::filesystem::path& part : path) {
        if (found) {
            rest /= part;
            continue;
        }
        prefix /= part;
        struct stat info{};
        if (::stat(prefix.c_str(), &info) != 0) {
            return std::nullopt;
        }
        found = S_ISREG(info.st_mode);
    }
    if (!found) {
        return std::nullopt;
    }
    std::optional<std::string> inner = clean_inner_path(rest.string());
    if (!inner) {
        return std::nullopt;
    }
    return ArchiveLocation{.archive = prefix, .inner = std::move(*inner)};
}

std::optional<std::string> clean_inner_path(std::string_view path) {
    if (path.starts_with('/') || path.size() > 1024)
        return std::nullopt;
    std::string clean;
    std::size_t depth = 0;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, slash - at);
        at = slash + 1;
        if (part.empty() || part == ".") {
            continue;
        }
        if (++depth > 64 || part == ".." || part.size() > kMaxName ||
            part.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        if (!clean.empty()) {
            clean += '/';
        }
        clean += part;
    }
    return clean;
}

ArchiveTree::ArchiveTree(archive::Listing listing, std::string_view own_name,
                         const MimeDatabase* mime)
    : format_{listing.format},
      failure_{listing.failure},
      detail_{std::move(listing.detail)},
      entries_{std::move(listing.entries)} {
    // Bound synthetic parents as well as explicit members before building the UI tree.
    // A small listing of deeply nested paths must not expand without a parent-process budget.
    std::unordered_set<std::string> nodes;
    std::size_t path_bytes = 0;
    for (const auto& entry : entries_) {
        auto clean = clean_inner_path(entry.path.empty() && format_ == archive::Format::gzip
                                          ? own_name
                                          : std::string_view{entry.path});
        if (!clean)
            continue;
        std::string path = std::move(*clean);
        while (!path.empty()) {
            if (nodes.insert(path).second)
                path_bytes += path.size();
            if (nodes.size() > 100'000 || path_bytes > 16 * 1024 * 1024) {
                entries_.clear();
                failure_ = archive::Failure::too_large;
                detail_ = "too many archive entries or folder paths to browse";
                folders_[""];
                return;
            }
            path = split_last(path).first;
        }
    }
    nodes.clear();
    folders_[""];  // the top, even when empty
    // Each folder's entries by name, while they are gathered.
    std::unordered_map<std::string, std::unordered_map<std::string, Entry>> gathered;
    const auto typed = [mime](Entry entry) {
        if (mime == nullptr) {
            return entry;
        }
        if (entry.is_directory()) {
            entry.mime = MimeDatabase::kDirectory;
            return entry;
        }
        // From the name only: the entry's bytes are not on disk to be looked at.
        const MimeDatabase::NameMatch match = mime->by_name(entry.name);
        entry.mime = match.mime != kUnknownMime ? match.mime : MimeDatabase::kOctetStream;
        entry.name_guess = match.certain ? kUnknownMime : match.mime;
        return entry;
    };
    // A folder and every folder above it, made up when the archive has no entry for them.
    const auto make_folders = [&](const std::string& path) {
        std::string folder = path;
        while (!folder.empty()) {
            auto [parent, name] = split_last(folder);
            std::unordered_map<std::string, Entry>& siblings = gathered[parent];
            const auto it = siblings.find(name);
            if (it == siblings.end()) {
                Entry made;
                made.name = name;
                made.kind = EntryKind::directory;
                made.mode = 0755;
                siblings.emplace(name, typed(std::move(made)));
            } else if (!it->second.is_directory()) {
                ++left_out_;  // a file where a folder is: the folder wins
                Entry made;
                made.name = name;
                made.kind = EntryKind::directory;
                made.mode = 0755;
                it->second = typed(std::move(made));
                by_path_.erase(folder);
            }
            folder = std::move(parent);
        }
    };
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const archive::Entry& item = entries_[i];
        std::string raw = item.path;
        if (raw.empty() && format_ == archive::Format::gzip) {
            raw = own_name;
        }
        const std::optional<std::string> path = clean_inner_path(raw);
        if (!path || path->empty()) {
            ++left_out_;
            continue;
        }
        auto [parent, name] = split_last(*path);
        make_folders(parent);
        Entry entry;
        entry.name = name;
        entry.mode = static_cast<std::uint16_t>(item.mode & 07777U);
        entry.modified_ns = nanoseconds(item.mtime);
        switch (item.type) {
            case archive::Type::directory:
                entry.kind = EntryKind::directory;
                break;
            case archive::Type::file:
            case archive::Type::hardlink:
                entry.kind = EntryKind::file;
                entry.size = item.size;
                break;
            case archive::Type::symlink:
                entry.kind = EntryKind::other;
                entry.is_link = true;
                break;
            case archive::Type::other:
                entry.kind = EntryKind::other;
                break;
        }
        std::unordered_map<std::string, Entry>& siblings = gathered[parent];
        const auto it = siblings.find(name);
        if (it != siblings.end() && it->second.is_directory() && !entry.is_directory()) {
            ++left_out_;  // a file where a folder is: the folder wins
            continue;
        }
        if (it != siblings.end() && it->second.is_directory() && entry.is_directory()) {
            it->second.mode = entry.mode;  // the archive's own entry for a folder made up before
            it->second.modified_ns = entry.modified_ns;
            by_path_[*path] = i;
            continue;
        }
        siblings.insert_or_assign(name, typed(std::move(entry)));  // a later one wins, as in tar
        by_path_[*path] = i;
    }
    for (auto& [folder, children] : gathered) {
        std::vector<Entry>& list = folders_[folder];
        list.reserve(children.size());
        for (auto& [name, entry] : children) {
            list.push_back(std::move(entry));
        }
        // Every folder named is listed, even with nothing in it.
        for (const Entry& entry : list) {
            if (entry.is_directory()) {
                folders_[folder.empty() ? entry.name : folder + "/" + entry.name];
            }
        }
    }
    // What it holds, as shown.
    for (const auto& [folder, list] : folders_) {
        for (const Entry& entry : list) {
            if (entry.is_directory()) {
                ++totals_.folders;
                continue;
            }
            if (entry.is_link) {
                ++totals_.links;
            } else if (entry.kind == EntryKind::file) {
                ++totals_.files;
                totals_.bytes += entry.size;
            } else {
                ++totals_.others;
            }
            const auto it = by_path_.find(folder.empty() ? entry.name : folder + "/" + entry.name);
            if (it == by_path_.end()) {
                continue;
            }
            const archive::Unreadable unreadable = entries_[it->second].unreadable;
            totals_.encrypted += unreadable == archive::Unreadable::encrypted ? 1 : 0;
            totals_.unsupported += unreadable == archive::Unreadable::method ? 1 : 0;
        }
    }
}

namespace {
std::optional<std::array<std::uint64_t, 7>> file_identity(const std::filesystem::path& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
    return std::array<std::uint64_t, 7>{static_cast<std::uint64_t>(info.st_dev),
                                        static_cast<std::uint64_t>(info.st_ino),
                                        static_cast<std::uint64_t>(info.st_size),
                                        static_cast<std::uint64_t>(info.st_mtim.tv_sec),
                                        static_cast<std::uint64_t>(info.st_mtim.tv_nsec),
                                        static_cast<std::uint64_t>(info.st_ctim.tv_sec),
                                        static_cast<std::uint64_t>(info.st_ctim.tv_nsec)};
}
}  // namespace

void ArchiveTree::identify(const std::filesystem::path& path) { identity_ = file_identity(path); }

bool ArchiveTree::unchanged(const std::filesystem::path& path) const {
    return identity_ && identity_ == file_identity(path);
}

const std::vector<Entry>* ArchiveTree::folder(std::string_view inner) const {
    const auto it = folders_.find(std::string{inner});
    return it == folders_.end() ? nullptr : &it->second;
}

const archive::Entry* ArchiveTree::entry(std::string_view path) const {
    const auto it = by_path_.find(std::string{path});
    return it == by_path_.end() ? nullptr : &entries_[it->second];
}

void ArchiveTree::each_under(
    std::string_view path,
    const std::function<void(const std::string& path, const archive::Entry* entry)>& visit) const {
    const auto under = [&](const std::string& each) {
        return path.empty() || each == path ||
               (each.size() > path.size() && each.starts_with(path) && each[path.size()] == '/');
    };
    for (const auto& [each, index] : by_path_) {
        if (under(each)) {
            visit(each, &entries_[index]);
        }
    }
    for (const auto& [each, list] : folders_) {
        if (!each.empty() && under(each) && !by_path_.contains(each)) {
            visit(each, nullptr);
        }
    }
}

namespace {

struct Kept {
    std::filesystem::path archive;
    dev_t device{0};
    ino_t inode{0};
    off_t size{0};
    std::int64_t mtime_ns{0};
    const MimeDatabase* mime{nullptr};  // its entries typed with this (none: untyped)
    std::shared_ptr<const ArchiveTree> tree;
};

std::mutex kept_mutex;
std::vector<Kept> kept;  // newest last; guarded by kept_mutex

}  // namespace

std::shared_ptr<const ArchiveTree> archive_tree(const std::filesystem::path& archive,
                                                const MimeDatabase* mime) {
    struct stat info{};
    if (::stat(archive.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::make_shared<const ArchiveTree>(
            archive::Listing{.format = archive::Format::unknown,
                             .failure = archive::Failure::unreadable,
                             .detail = "it could not be read",
                             .entries = {}},
            "", mime);
    }
    const std::int64_t mtime_ns =
        (static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000) + info.st_mtim.tv_nsec;
    const auto same = [&](const Kept& each) {
        return each.archive == archive && each.device == info.st_dev && each.inode == info.st_ino &&
               each.size == info.st_size && each.mtime_ns == mtime_ns &&
               (each.mime == mime || mime == nullptr);  // untyped: any will do
    };
    {
        const std::scoped_lock lock{kept_mutex};
        const auto it = std::ranges::find_if(kept, same);
        if (it != kept.end() && it->tree->unchanged(archive)) {
            return it->tree;
        }
    }
    std::string own_name = archive.filename().string();
    if (own_name.size() > 3 && own_name.ends_with(".gz")) {
        own_name.resize(own_name.size() - 3);
    }
    const auto before = file_identity(archive);
    auto listing = archive::list_archive(archive);
    if (before != file_identity(archive)) {
        listing.failure = archive::Failure::unreadable;
        listing.detail = "archive changed while being listed";
        listing.entries.clear();
    }
    auto tree = std::make_shared<ArchiveTree>(std::move(listing), own_name, mime);
    tree->identify(archive);
    if (before != file_identity(archive)) {
        return std::make_shared<const ArchiveTree>(
            archive::Listing{.format = archive::Format::unknown,
                             .failure = archive::Failure::unreadable,
                             .detail = "archive changed while being listed",
                             .entries = {}},
            "", mime);
    }
    if (tree->failure() == archive::Failure::none) {
        const std::scoped_lock lock{kept_mutex};
        std::erase_if(
            kept, [&](const Kept& each) { return each.archive == archive && each.mime == mime; });
        kept.push_back({.archive = archive,
                        .device = info.st_dev,
                        .inode = info.st_ino,
                        .size = info.st_size,
                        .mtime_ns = mtime_ns,
                        .mime = mime,
                        .tree = tree});
        if (kept.size() > kKeptTrees) {
            kept.erase(kept.begin());
        }
    }
    return tree;
}

void forget_archive_trees() {
    const std::scoped_lock lock{kept_mutex};
    kept.clear();
}

}  // namespace rummage::fs
