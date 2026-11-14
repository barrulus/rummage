#include "rummage/fs/directory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <memory>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

#include "rummage/fs/folder_icon.hpp"

namespace rummage::fs {
namespace {

// Entries between cancellation checks.
constexpr std::size_t kCancelCheckInterval = 64;

EntryKind kind_of(mode_t mode) {
    if (S_ISREG(mode)) {
        return EntryKind::file;
    }
    if (S_ISDIR(mode)) {
        return EntryKind::directory;
    }
    return EntryKind::other;
}

struct DirCloser {
    void operator()(DIR* dir) const noexcept { closedir(dir); }
};

}  // namespace

constexpr std::size_t kNameBuffer = 4096;

char type_letter(mode_t mode) {
    if (S_ISDIR(mode)) {
        return 'd';
    }
    if (S_ISLNK(mode)) {
        return 'l';
    }
    if (S_ISCHR(mode)) {
        return 'c';
    }
    if (S_ISBLK(mode)) {
        return 'b';
    }
    if (S_ISFIFO(mode)) {
        return 'p';
    }
    if (S_ISSOCK(mode)) {
        return 's';
    }
    return '-';
}

// As ls -l: "drwxr-xr-x", with s/S for setuid and setgid, t/T for sticky.
std::string file_permissions(std::uint32_t mode) {
    std::string out(10, '-');
    out[0] = type_letter(mode);
    constexpr std::array<mode_t, 9> bits{S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                                         S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    constexpr std::string_view letters = "rwxrwxrwx";
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if ((mode & bits.at(i)) != 0) {
            out[i + 1] = letters[i];
        }
    }
    const auto special = [&](std::size_t at, mode_t bit, char set, char set_no_exec) {
        if ((mode & bit) != 0) {
            out[at] = out[at] == 'x' ? set : set_no_exec;
        }
    };
    special(3, S_ISUID, 's', 'S');
    special(6, S_ISGID, 's', 'S');
    special(9, S_ISVTX, 't', 'T');
    return out;
}

std::string owner_name(std::uint32_t uid) {
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(kNameBuffer);
    if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return entry.pw_name;
    }
    return std::to_string(uid);
}

std::string group_name(std::uint32_t gid) {
    group entry{};
    group* found = nullptr;
    std::vector<char> buffer(kNameBuffer);
    if (::getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return entry.gr_name;
    }
    return std::to_string(gid);
}

namespace {

// One metadata query includes birth time where available. Older kernels keep working
// through fstatat, with creation time unavailable.
bool stat_info(int dir_fd, const char* name, int flags, struct statx& info) {
    if (::statx(dir_fd, name, flags, STATX_BASIC_STATS | STATX_BTIME, &info) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL && errno != EPERM)
        return false;
    struct stat old{};
    if (::fstatat(dir_fd, name, &old, flags) != 0)
        return false;
    info.stx_mask = STATX_BASIC_STATS;
    info.stx_mode = static_cast<std::uint16_t>(old.st_mode);
    info.stx_uid = old.st_uid;
    info.stx_gid = old.st_gid;
    info.stx_size = static_cast<std::uint64_t>(old.st_size);
    info.stx_atime.tv_sec = old.st_atim.tv_sec;
    info.stx_atime.tv_nsec = static_cast<std::uint32_t>(old.st_atim.tv_nsec);
    info.stx_mtime.tv_sec = old.st_mtim.tv_sec;
    info.stx_mtime.tv_nsec = static_cast<std::uint32_t>(old.st_mtim.tv_nsec);
    return true;
}

struct Identities {
    std::unordered_map<uid_t, std::string> users;
    std::unordered_map<gid_t, std::string> groups;

    void fill(Entry& entry, const struct statx& info) {
        auto user = users.find(info.stx_uid);
        if (user == users.end())
            user = users.emplace(info.stx_uid, owner_name(info.stx_uid)).first;
        auto group = groups.find(info.stx_gid);
        if (group == groups.end())
            group = groups.emplace(info.stx_gid, group_name(info.stx_gid)).first;
        entry.owner = user->second;
        entry.group = group->second;
        entry.permissions = file_permissions(info.stx_mode);
        entry.accessed_ns = (info.stx_atime.tv_sec * 1'000'000'000) + info.stx_atime.tv_nsec;
    }
};

// Fills kind, size and time; false when the entry is gone.
bool stat_entry(int dir_fd, Entry& entry, Identities& identities) {
    struct statx info{};
    if (!stat_info(dir_fd, entry.name.c_str(), AT_SYMLINK_NOFOLLOW, info)) {
        return errno != ENOENT;  // keep it with unknown kind on other errors (EACCES, ...)
    }
    if (S_ISLNK(info.stx_mode)) {
        entry.is_link = true;
        std::vector<char> buffer(256);
        for (;;) {
            const auto length =
                readlinkat(dir_fd, entry.name.c_str(), buffer.data(), buffer.size());
            if (length < 0)
                break;
            if (static_cast<std::size_t>(length) < buffer.size()) {
                entry.link_target.assign(buffer.data(), static_cast<std::size_t>(length));
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        struct statx target{};
        if (stat_info(dir_fd, entry.name.c_str(), 0, target))
            info = target;
        // A dangling link keeps its own metadata.
    }
    identities.fill(entry, info);
    if ((info.stx_mask & STATX_BTIME) != 0) {
        entry.created_ns = (info.stx_btime.tv_sec * 1'000'000'000) + info.stx_btime.tv_nsec;
    }
    entry.kind = kind_of(info.stx_mode);
    entry.size = static_cast<std::uint64_t>(info.stx_size);
    entry.modified_ns = (info.stx_mtime.tv_sec * 1'000'000'000) + info.stx_mtime.tv_nsec;
    return true;
}

// The type when the kind or the name decides it (no file read), else the name's best guess.
void type_without_content(const MimeDatabase& db, Entry& entry) {
    switch (entry.kind) {
        case EntryKind::directory:
            entry.mime = MimeDatabase::kDirectory;
            return;
        case EntryKind::other:
            entry.mime = entry.is_link ? MimeDatabase::kSymlink : MimeDatabase::kSpecial;
            return;
        case EntryKind::file:
        case EntryKind::unknown:
            break;
    }
    if (entry.size == 0) {
        entry.mime = MimeDatabase::kZeroSize;
        return;
    }
    const MimeDatabase::NameMatch match = db.by_name(entry.name);
    (match.certain ? entry.mime : entry.name_guess) = match.mime;
}

}  // namespace

ReadResult read_directory(const std::filesystem::path& path, const core::CancelToken& token,
                          const ReadOptions& options,
                          const std::function<void(std::vector<Entry> chunk)>& on_chunk) {
    ReadResult result;
    const std::unique_ptr<DIR, DirCloser> dir{opendir(path.c_str())};
    if (!dir) {
        result.error = {errno, std::generic_category()};
        return result;
    }
    const int dir_fd = dirfd(dir.get());
    const FolderIcons icons;
    Identities identities;
    std::size_t chunk_size = std::max<std::size_t>(options.first_chunk, 1);
    std::vector<Entry> chunk;
    chunk.reserve(chunk_size);
    std::size_t seen = 0;
    for (;;) {
        errno = 0;
        const dirent* item = readdir(dir.get());
        if (item == nullptr) {
            if (errno != 0) {
                result.error = {errno, std::generic_category()};
            }
            break;
        }
        if (++seen % kCancelCheckInterval == 0 && token.cancelled()) {
            return result;
        }
        const std::string_view name{static_cast<const char*>(item->d_name)};
        if (name == "." || name == "..") {
            continue;
        }
        Entry entry{.name = std::string{name}};
        if (!stat_entry(dir_fd, entry, identities)) {
            continue;
        }
        if (options.mime) {
            type_without_content(*options.mime, entry);
        }
        // Avoid probing zero-sized autofs placeholders (which could mount other homes).
        if (entry.is_directory() && entry.size != 0) {
            entry.folder_icon = icons.for_folder(path / entry.name);
        }
        chunk.push_back(std::move(entry));
        if (chunk.size() == chunk_size) {
            if (token.cancelled()) {
                return result;
            }
            result.count += chunk.size();
            on_chunk(std::move(chunk));
            chunk_size = std::min(chunk_size * 2, std::max(options.max_chunk, chunk_size));
            chunk = {};
            chunk.reserve(chunk_size);
        }
    }
    if (!chunk.empty() && !token.cancelled()) {
        result.count += chunk.size();
        on_chunk(std::move(chunk));
    }
    return result;
}

std::vector<std::optional<Entry>> read_entries(const std::filesystem::path& path,
                                               const std::vector<std::string>& names,
                                               const ReadOptions& options) {
    std::vector<std::optional<Entry>> result(names.size());
    const int dir_fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return result;
    }
    const FolderIcons icons;
    Identities identities;
    for (std::size_t i = 0; i < names.size(); ++i) {
        Entry entry{.name = names[i]};
        if (!stat_entry(dir_fd, entry, identities)) {
            continue;  // gone
        }
        if (options.mime) {
            type_without_content(*options.mime, entry);
        }
        if (entry.is_directory() && entry.size != 0) {
            entry.folder_icon = icons.for_folder(path / entry.name);
        }
        result[i] = std::move(entry);
    }
    close(dir_fd);
    return result;
}

core::CancelToken load_directory(core::WorkerPool& pool, std::filesystem::path path,
                                 std::function<void(std::vector<Entry> chunk)> on_chunk,
                                 std::function<void(const ReadResult& result)> on_done,
                                 ReadOptions options) {
    return pool.submit_streaming(
        core::Priority::visible,
        [path = std::move(path), options](const core::CancelToken& token, auto& emit) {
            return read_directory(path, token, options,
                                  [&emit](std::vector<Entry> chunk) { emit(std::move(chunk)); });
        },
        [on_chunk = std::move(on_chunk)](std::vector<Entry> chunk) { on_chunk(std::move(chunk)); },
        [on_done = std::move(on_done)](const ReadResult& result) { on_done(result); });
}

}  // namespace rummage::fs
