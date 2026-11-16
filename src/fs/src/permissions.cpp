#include "rummage/fs/permissions.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace rummage::fs {
namespace {

constexpr std::uint16_t kAnyExecute = S_IXUSR | S_IXGRP | S_IXOTH;

std::string error_text(const std::filesystem::path& path, int code) {
    return std::format("{}: {}", path.filename().string(),
                       std::error_code{code, std::generic_category()}.message());
}

}  // namespace

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
