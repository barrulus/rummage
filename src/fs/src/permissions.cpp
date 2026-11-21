#include "rummage/fs/permissions.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <format>
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
