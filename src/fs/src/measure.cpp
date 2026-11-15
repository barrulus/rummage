#include "rummage/fs/measure.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/fs/places.hpp"

namespace rummage::fs {
namespace {

// statfs's f_type for the file systems is_remote turns away (linux/magic.h and the file systems'
// own headers; written out here, as not every one is in linux/magic.h).
constexpr std::uint32_t kFuse = 0x65735546;
constexpr std::uint64_t kTellEvery = 256;  // entries between looks at the clock (progress)
constexpr std::array<std::uint32_t, 10> kRemote{
    0x6969,      // NFS
    0x517B,      // SMB (smbfs)
    0xFF534D42,  // CIFS
    0xFE534D42,  // SMB2
    0x01021997,  // 9P (v9fs)
    0x00C36400,  // Ceph
    0x5346414F,  // OpenAFS
    0x6B414653,  // kAFS
    0x73757245,  // Coda
    0x564C,      // NCP
};

// The mount's type in mountinfo's text for the device major:minor ("fuse.sshfs", "fuseblk");
// empty when it is not there.
std::string mount_type_of(std::string_view mountinfo, dev_t device) {
    const std::string wanted = std::format("{}:{}", major(device), minor(device));
    while (!mountinfo.empty()) {
        const std::size_t end = mountinfo.find('\n');
        const std::string_view line = mountinfo.substr(0, end);
        mountinfo = end == std::string_view::npos ? std::string_view{} : mountinfo.substr(end + 1);
        // "36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw"
        std::string_view rest = line;
        for (int skip = 0; skip < 2 && !rest.empty(); ++skip) {
            const std::size_t space = rest.find(' ');
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        }
        if (rest.substr(0, rest.find(' ')) != wanted) {
            continue;
        }
        const std::size_t dash = line.find(" - ");
        if (dash == std::string_view::npos) {
            return {};
        }
        const std::string_view type = line.substr(dash + 3);
        return std::string{type.substr(0, type.find(' '))};
    }
    return {};
}

}  // namespace

bool is_remote_type(std::int64_t magic, std::string_view mount_type) {
    const auto type = static_cast<std::uint32_t>(magic);
    if (type == kFuse) {
        return mount_type != "fuseblk";
    }
    return std::ranges::find(kRemote, type) != kRemote.end();
}

bool is_remote(const std::filesystem::path& path) {
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return false;  // nothing to walk anyway
    }
    std::string mount_type;
    if (std::cmp_equal(static_cast<std::uint32_t>(info.f_type), kFuse)) {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) {
            mount_type = mount_type_of(read_proc_file("/proc/self/mountinfo"), st.st_dev);
        }
    }
    return is_remote_type(info.f_type, mount_type);
}

Measurer::Measurer(const MeasureOptions& options, const core::CancelToken& token, Progress progress)
    : options_{options},
      token_{token},
      progress_{std::move(progress)},
      told_{std::chrono::steady_clock::now()} {}

DIR* Measurer::visit(int parent, const char* name, bool follow, std::size_t depth,
                     const dev_t* device) {
    struct stat info{};
    if (fstatat(parent, name, &info, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
        ++tally_.unreadable;
        return nullptr;
    }
    if (S_ISREG(info.st_mode)) {
        ++tally_.files;
        if (linked_.emplace(info.st_dev, info.st_ino).second) {
            tally_.bytes += static_cast<std::uint64_t>(info.st_size);
        }
        return nullptr;
    }
    if (!S_ISDIR(info.st_mode)) {
        ++tally_.others;
        return nullptr;
    }
    if (directories_.contains({info.st_dev, info.st_ino})) {
        return nullptr;
    }
    ++tally_.folders;
    if ((device != nullptr && options_.one_file_system && info.st_dev != *device) ||
        depth >= options_.max_depth) {
        ++tally_.not_entered;
        return nullptr;
    }
    const int fd =
        openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0) {
        ++tally_.unreadable;
        return nullptr;
    }
    // Check the opened object too: a path may have been replaced since fstatat.
    struct stat opened{};
    if (fstat(fd, &opened) != 0 || opened.st_dev != info.st_dev || opened.st_ino != info.st_ino) {
        close(fd);
        ++tally_.unreadable;
        return nullptr;
    }
    DIR* const dir = fdopendir(fd);
    if (dir == nullptr) {
        close(fd);
        ++tally_.unreadable;
    } else {
        directories_.emplace(info.st_dev, info.st_ino);
    }
    return dir;
}

void Measurer::add(const std::filesystem::path& path) {
    if (token_.cancelled()) {
        tally_.cancelled = true;
        return;
    }
    if (options_.skip_remote && is_remote(path)) {
        tally_.remote = true;
        return;
    }
    DIR* const root = visit(AT_FDCWD, path.c_str(), options_.follow_root, 0, nullptr);
    if (root == nullptr) {
        return;
    }
    struct stat info{};
    if (fstat(dirfd(root), &info) != 0) {
        ++tally_.unreadable;
        closedir(root);
        return;
    }
    const dev_t device = info.st_dev;
    // The folders being listed, outermost first: one open folder per level.
    std::vector<DIR*> open{root};
    while (!open.empty()) {
        if (token_.cancelled()) {
            tally_.cancelled = true;
            break;
        }
        DIR* const dir = open.back();
        errno = 0;
        const dirent* const entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0) {
                ++tally_.unreadable;
            }
            closedir(dir);
            open.pop_back();
            continue;
        }
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        if (DIR* const child = visit(dirfd(dir), entry->d_name, false, open.size(), &device)) {
            open.push_back(child);
        }
        tell();
    }
    for (DIR* const dir : open) {
        closedir(dir);
    }
}

void Measurer::tell() {
    if (!progress_ || tally_.entries() % kTellEvery != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - told_ < options_.progress_interval) {
        return;
    }
    told_ = now;
    progress_(tally_);
}

Tally measure(const std::filesystem::path& path, const MeasureOptions& options,
              const core::CancelToken& token, const Measurer::Progress& progress) {
    Measurer measurer{options, token, progress};
    measurer.add(path);
    return measurer.tally();
}

}  // namespace rummage::fs
