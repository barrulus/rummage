#include "rummage/fs/archive_origin.hpp"

#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "rummage/core/file.hpp"

namespace rummage::fs {
namespace {
std::error_code error() { return {errno, std::generic_category()}; }

bool absent(int code) { return code == ENODATA || code == ENOTSUP; }

std::error_code remove_execution(int fd) {
    struct stat info{};
    if (fstat(fd, &info) != 0 || fchmod(fd, info.st_mode & 0666) != 0)
        return error();
    return {};
}

std::error_code walk(int fd, std::string_view origin, const core::CancelToken& token,
                     unsigned depth, std::size_t& count) {
    if (token.cancelled())
        return std::make_error_code(std::errc::operation_canceled);
    if (depth > 64 || ++count > 1'000'000)
        return std::make_error_code(std::errc::value_too_large);
    struct stat info{};
    if (fstat(fd, &info) != 0)
        return error();
    if (S_ISREG(info.st_mode))
        return mark_archive_origin(fd, origin);
    if (!S_ISDIR(info.st_mode))
        return {};
    DIR* dir = fdopendir(dup(fd));
    if (!dir)
        return error();
    std::error_code result;
    for (;;) {
        errno = 0;
        const dirent* entry = readdir(dir);
        if (!entry) {
            if (errno != 0)
                result = error();
            break;
        }
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..")
            continue;
        struct stat child{};
        if (fstatat(fd, entry->d_name, &child, AT_SYMLINK_NOFOLLOW) != 0) {
            result = error();
            break;
        }
        if (!S_ISDIR(child.st_mode) && !S_ISREG(child.st_mode))
            continue;
        const int nested =
            openat(fd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (nested < 0) {
            result = error();
            break;
        }
        result = walk(nested, origin, token, depth + 1, count);
        close(nested);
        if (result)
            break;
    }
    closedir(dir);
    return result;
}
}  // namespace

std::error_code mark_archive_origin(int fd, std::string_view origin) {
    // Never store an unbounded URL received from an untrusted xattr.
    if (origin.empty() || origin.size() > 4096)
        origin = "archive";
    if (fsetxattr(fd, kArchiveOrigin, origin.data(), origin.size(), 0) == 0)
        return {};
    return remove_execution(fd);
}

std::error_code copy_archive_origin(int source, int destination) {
    std::array<char, 4096> data{};
    const ssize_t size = fgetxattr(source, kArchiveOrigin, data.data(), data.size());
    if (size >= 0)
        return mark_archive_origin(destination, {data.data(), static_cast<std::size_t>(size)});
    if (absent(errno))
        return {};
    // Unreadable or overlong provenance cannot silently turn into trusted executable data.
    return mark_archive_origin(destination, "archive");
}

bool archive_confirmation_required(const std::filesystem::path& file) {
    const ssize_t size = lgetxattr(file.c_str(), kArchiveOrigin, nullptr, 0);
    return size >= 0 || !absent(errno);
}

bool archive_confirmation_required(int fd) {
    const ssize_t size = fgetxattr(fd, kArchiveOrigin, nullptr, 0);
    return size >= 0 || !absent(errno);
}

std::string archive_origin(const std::filesystem::path& archive) {
    std::array<char, 4096> data{};
    const ssize_t size = getxattr(archive.c_str(), "user.xdg.origin.url", data.data(), data.size());
    if (size > 0)
        return {data.data(), static_cast<std::size_t>(size)};
    return core::file_uri(archive);
}

std::error_code mark_archive_tree(const std::filesystem::path& path, std::string_view origin,
                                  const core::CancelToken& token) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return error();
    std::size_t count = 0;
    const auto result = walk(fd, origin, token, 0, count);
    close(fd);
    return result;
}

std::error_code approve_archive_program(int fd) {
    struct stat info{};
    if (fstat(fd, &info) != 0)
        return error();
    if (!S_ISREG(info.st_mode))
        return std::make_error_code(std::errc::invalid_argument);
    if (fchmod(fd, (info.st_mode & 0777) | S_IXUSR) != 0)
        return error();
    if (fremovexattr(fd, kArchiveOrigin) != 0 && !absent(errno))
        return error();
    return {};
}
}  // namespace rummage::fs
