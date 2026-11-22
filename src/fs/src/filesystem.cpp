#include "rummage/fs/filesystem.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/statvfs.h>
#include <unistd.h>

#include "rummage/fs/places.hpp"

namespace rummage::fs {
namespace {
std::string unescape(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7' &&
            text[i + 2] >= '0' && text[i + 2] <= '7' && text[i + 3] >= '0' && text[i + 3] <= '7') {
            out += static_cast<char>((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 +
                                     text[i + 3] - '0');
            i += 3;
        } else
            out += text[i];
    }
    return out;
}

std::uint64_t bytes(std::uint64_t blocks, std::uint64_t size) {
    return size && blocks > std::numeric_limits<std::uint64_t>::max() / size
               ? std::numeric_limits<std::uint64_t>::max()
               : blocks * size;
}
}  // namespace

FilesystemFacts filesystem_mount(std::string_view mountinfo, const std::filesystem::path& path) {
    FilesystemFacts result;
    std::istringstream lines{std::string{mountinfo}};
    std::string line;
    for (; std::getline(lines, line);) {
        std::istringstream fields{line};
        std::string id, parent, device, root, mount, options, word, type, source;
        if (!(fields >> id >> parent >> device >> root >> mount >> options))
            continue;
        while (fields >> word && word != "-") {
        }
        if (word != "-" || !(fields >> type >> source))
            continue;
        const std::filesystem::path point{unescape(mount)};
        const auto relative = path.lexically_relative(point);
        if (!point.is_absolute() || relative.empty() || *relative.begin() == "..")
            continue;
        if (point.native().size() < result.mount_point.native().size())
            continue;
        result.mount_point = point;
        result.type = type;
        result.device = unescape(source);
    }
    return result;
}

FilesystemFacts filesystem_facts(const std::filesystem::path& path) {
    FilesystemFacts result;
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        result.error = {errno, std::generic_category()};
        return result;
    }
    struct statvfs info{};
    const int status = ::fstatvfs(fd, &info);
    const int error = errno;
    std::error_code ignored;
    const auto resolved =
        std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd), ignored);
    if (!ignored)
        result = filesystem_mount(read_proc_file("/proc/self/mountinfo"), resolved);
    ::close(fd);
    if (status != 0) {
        result.error = {error, std::generic_category()};
        return result;
    }
    result.capacity = bytes(info.f_blocks, info.f_frsize);
    result.free = std::min(result.capacity, bytes(info.f_bfree, info.f_frsize));
    result.available = std::min(result.free, bytes(info.f_bavail, info.f_frsize));
    result.read_only = (info.f_flag & ST_RDONLY) != 0;
    return result;
}
}  // namespace rummage::fs
