#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "rummage/fs/entry.hpp"
#include "rummage/preview/preview.hpp"

namespace rummage::preview {
namespace {

constexpr std::uint64_t kCancelCheck = 4096;  // entries counted between looks at the token

}  // namespace

Info read_info(const std::filesystem::path& path, const core::CancelToken& cancel) {
    Info info;
    struct stat link{};
    if (::lstat(path.c_str(), &link) != 0) {
        info.error = std::strerror(errno);  // NOLINT(concurrency-mt-unsafe): glibc's is safe
        return info;
    }
    struct stat target = link;
    if (S_ISLNK(link.st_mode)) {
        std::error_code error;
        info.link_target = std::filesystem::read_symlink(path, error).string();
        if (::stat(path.c_str(), &target) != 0) {
            target = link;  // dangling: the link's own
        }
    }
    struct statx birth{};
    // NOLINTNEXTLINE(hicpp-signed-bitwise): flags as the kernel defines them
    if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &birth) == 0 &&
        (birth.stx_mask & STATX_BTIME) != 0) {
        info.created_ns = (static_cast<std::int64_t>(birth.stx_btime.tv_sec) * 1'000'000'000) +
                          birth.stx_btime.tv_nsec;
    }
    info.permissions = fs::file_permissions(target.st_mode);
    info.owner = fs::owner_name(target.st_uid);
    info.group = fs::group_name(target.st_gid);
    if (S_ISDIR(target.st_mode)) {
        std::error_code error;
        std::filesystem::directory_iterator it{path, error};
        if (!error) {
            std::uint64_t count = 0;
            std::uint64_t hidden = 0;
            for (; it != std::filesystem::directory_iterator{}; it.increment(error)) {
                if (error || (++count % kCancelCheck == 0 && cancel.cancelled())) {
                    break;
                }
                if (it->path().filename().native().starts_with('.')) {
                    ++hidden;
                }
            }
            if (!error) {
                info.items = count;
                info.hidden_items = hidden;
            }
        }
    }
    return info;
}

std::string permissions_text(std::uint32_t mode) {
    return fs::file_permissions(static_cast<mode_t>(mode));
}

}  // namespace rummage::preview
