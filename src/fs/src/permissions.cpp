#include "rummage/fs/permissions.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <grp.h>
#include <memory>
#include <pwd.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace rummage::fs {
namespace {

constexpr std::uint16_t kAnyExecute = S_IXUSR | S_IXGRP | S_IXOTH;

std::string error_text(const std::filesystem::path& path, int code) {
    return std::format("{}: {}", path.filename().string(),
                       std::error_code{code, std::generic_category()}.message());
}

}  // namespace

std::string set_permissions(const std::filesystem::path& path, std::uint64_t device,
                            std::uint64_t inode, std::uint16_t before, std::uint16_t permissions) {
    // O_PATH also works when the owner has removed all read/write permissions. /proc/self/fd
    // addresses this pinned inode, so replacing the pathname cannot redirect chmod.
    const int fd = ::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return error_text(path, errno);
    struct stat info{};
    std::string error;
    if (::fstat(fd, &info) != 0)
        error = error_text(path, errno);
    else if ((!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_dev != device ||
             info.st_ino != inode)
        error = "The item was removed or replaced. Reopen Properties.";
    else if (info.st_uid != ::geteuid())
        error = error_text(path, EPERM);
    else if ((info.st_mode & 07777) != before)
        error = "Permissions changed since Properties was opened. Reopen Properties.";
    else if (::chmod(std::format("/proc/self/fd/{}", fd).c_str(),
                     (before & 07000) | (permissions & 0777)) != 0)
        error = error_text(path, errno);
    ::close(fd);
    return error;
}

PermissionTarget permission_target(const std::filesystem::path& path) {
    PermissionTarget target;
    target.path = path;
    while (target.path != target.path.root_path() && !target.path.has_filename())
        target.path = target.path.parent_path();
    struct stat info{};
    if (::lstat(target.path.c_str(), &info) != 0)
        target.error = error_text(path, errno);
    else if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
        target.error = path.filename().string() + ": symbolic links and special files are skipped";
    else {
        target.device = info.st_dev;
        target.inode = info.st_ino;
        target.owner = info.st_uid;
        target.group = info.st_gid;
        target.mode = static_cast<std::uint16_t>(info.st_mode & 07777);
        target.directory = S_ISDIR(info.st_mode);
    }
    return target;
}

PermissionResult apply_permissions(const std::vector<PermissionTarget>& roots,
                                   const PermissionEdit& edit,
                                   const std::function<bool()>& cancelled) {
    PermissionResult result;
    const auto resolve = [&](const std::string& text, bool user, std::optional<std::uint32_t>& id) {
        if (text.empty())
            return true;
        if (text.find('\0') != std::string::npos)
            return false;
        std::uint32_t number{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
            number != UINT32_MAX) {
            id = number;
            return true;
        }
        std::vector<char> buffer(65536);
        if (user) {
            passwd entry{}, *found{};
            if (::getpwnam_r(text.c_str(), &entry, buffer.data(), buffer.size(), &found) == 0 &&
                found)
                id = entry.pw_uid;
        } else {
            struct group entry{}, *found{};
            if (::getgrnam_r(text.c_str(), &entry, buffer.data(), buffer.size(), &found) == 0 &&
                found)
                id = entry.gr_gid;
        }
        return id.has_value();
    };
    std::optional<std::uint32_t> uid, gid;
    if (!resolve(edit.owner, true, uid) || !resolve(edit.group, false, gid)) {
        result.errors.push_back("Unknown owner or group; use an account name or numeric ID");
        return result;
    }
    std::set<std::pair<std::uint64_t, std::uint64_t>> visited, stale;
    std::size_t inspected{};
    bool stopped = false;
    const auto visit = [&](const auto& self, int fd, const std::filesystem::path& path,
                           std::uint64_t device, unsigned depth) -> void {
        if (stopped)
            return;
        if ((cancelled && cancelled()) || ++inspected > 100000 || depth > 256) {
            result.errors.push_back(
                "Permission changes stopped: cancelled or traversal limit reached");
            stopped = true;
            return;
        }
        struct stat before{};
        if (::fstat(fd, &before) != 0) {
            result.errors.push_back(error_text(path, errno));
            return;
        }
        if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode))
            return;  // child symlinks/special files deliberately never followed
        if (before.st_dev != device) {
            result.errors.push_back(path.string() + ": another filesystem; skipped");
            return;
        }
        if (!visited.emplace(before.st_dev, before.st_ino).second)
            return;
        if (edit.recursive && S_ISDIR(before.st_mode)) {
            const int directory = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            DIR* stream = directory < 0 ? nullptr : ::fdopendir(directory);
            if (!stream) {
                const int error = errno;
                if (directory >= 0)
                    ::close(directory);
                result.errors.push_back(error_text(path, error));
            } else {
                for (;;) {
                    errno = 0;
                    const auto* entry = ::readdir(stream);
                    if (!entry) {
                        if (errno)
                            result.errors.push_back(error_text(path, errno));
                        break;
                    }
                    const std::string name{entry->d_name};
                    if (name == "." || name == "..")
                        continue;
                    const int child =
                        ::openat(directory, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
                    if (child < 0)
                        result.errors.push_back(error_text(path / name, errno));
                    else {
                        self(self, child, path / name, device, depth + 1);
                        ::close(child);
                    }
                    if (stopped)
                        break;
                }
                ::closedir(stream);
            }
        }
        if (stopped)
            return;
        struct stat latest{};
        if (::fstat(fd, &latest) != 0 || latest.st_mode != before.st_mode ||
            latest.st_uid != before.st_uid || latest.st_gid != before.st_gid) {
            stale.emplace(before.st_dev, before.st_ino);
            result.errors.push_back(path.string() +
                                    ": permissions changed during traversal; skipped");
            return;
        }
        bool changed = false;
        const mode_t mask = static_cast<mode_t>(edit.mask & 0777);
        const auto wanted = (before.st_mode & 07777 & ~mask) | (edit.values & mask);
        if (wanted != (before.st_mode & 07777)) {
            if (::chmod(std::format("/proc/self/fd/{}", fd).c_str(), wanted) != 0)
                result.errors.push_back(error_text(path, errno));
            else
                changed = true;
        }
        if ((uid && *uid != before.st_uid) || (gid && *gid != before.st_gid)) {
            if (::fchownat(fd, "", uid.value_or(static_cast<uid_t>(-1)),
                           gid.value_or(static_cast<gid_t>(-1)), AT_EMPTY_PATH) != 0)
                result.errors.push_back(error_text(path, errno));
            else
                changed = true;
        }
        if (changed)
            ++result.changed;
    };
    for (const auto& root : roots) {
        if (stopped)
            break;
        if (!root.error.empty()) {
            result.errors.push_back(root.error);
            continue;
        }
        const int fd = ::open(root.path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            result.errors.push_back(error_text(root.path, errno));
            continue;
        }
        struct stat current{};
        if (::fstat(fd, &current) != 0)
            result.errors.push_back(error_text(root.path, errno));
        else if (current.st_dev != root.device || current.st_ino != root.inode ||
                 current.st_uid != root.owner || current.st_gid != root.group ||
                 (current.st_mode & 07777) != root.mode)
            result.errors.push_back(root.path.string() +
                                    ": changed since Properties opened; reopen it");
        else {
            visit(visit, fd, root.path, root.device, 0);
            if (!stale.contains({root.device, root.inode}) && ::fstat(fd, &current) == 0) {
                auto refreshed = root;
                refreshed.mode = static_cast<std::uint16_t>(current.st_mode & 07777);
                refreshed.owner = current.st_uid;
                refreshed.group = current.st_gid;
                result.refreshed.push_back(std::move(refreshed));
            }
        }
        ::close(fd);
    }
    return result;
}

std::uint16_t running_mode(std::uint16_t mode, bool allow) noexcept {
    return static_cast<std::uint16_t>(allow ? (mode | S_IXUSR) : (mode & ~kAnyExecute));
}

ModeResult set_running(const std::vector<std::filesystem::path>& files, bool allow) {
    ModeResult result;
    for (const std::filesystem::path& file : files) {
        struct stat info{};
        if (::stat(file.c_str(), &info) != 0) {
            result.errors.push_back(error_text(file, errno));
            continue;
        }
        if (!S_ISREG(info.st_mode)) {
            continue;  // a folder's execute bit means something else
        }
        const auto before = static_cast<std::uint16_t>(info.st_mode & 07777);
        const std::uint16_t after = running_mode(before, allow);
        if (after == before) {
            continue;
        }
        if (::chmod(file.c_str(), after) != 0) {
            result.errors.push_back(error_text(file, errno));
            continue;
        }
        result.changed.push_back({.path = file, .before = before, .after = after});
    }
    return result;
}

ModeResult restore_modes(const std::vector<ModeChange>& changes, bool before) {
    ModeResult result;
    for (const ModeChange& change : changes) {
        const std::uint16_t now = before ? change.after : change.before;
        const std::uint16_t wanted = before ? change.before : change.after;
        struct stat info{};
        if (::stat(change.path.c_str(), &info) != 0) {
            result.errors.push_back(error_text(change.path, errno));
            continue;
        }
        if (!S_ISREG(info.st_mode) || (info.st_mode & 07777) != now) {
            result.errors.push_back(
                std::format("{}: changed since", change.path.filename().string()));
            continue;
        }
        if (::chmod(change.path.c_str(), wanted) != 0) {
            result.errors.push_back(error_text(change.path, errno));
            continue;
        }
        result.changed.push_back({.path = change.path, .before = now, .after = wanted});
    }
    return result;
}

}  // namespace rummage::fs
