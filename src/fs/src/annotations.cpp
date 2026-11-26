#include "rummage/fs/annotations.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace rummage::fs {
namespace {
constexpr std::array<const char*, 3> kNames{"user.xdg.tags", "user.rummage.rating",
                                            "user.xdg.comment"};
constexpr std::size_t kLimit = 4096;
using Fields = std::array<std::string*, 3>;

Fields fields(Annotations& value) { return {&value.tags, &value.rating, &value.comment}; }

std::error_code error() { return {errno, std::generic_category()}; }

std::string pinned(int fd) { return std::format("/proc/self/fd/{}", fd); }

std::string validate(const Annotations& value) {
    for (const auto* text : {&value.tags, &value.rating, &value.comment})
        if (text->size() > kLimit || text->find('\0') != std::string::npos)
            return "Annotations must be at most 4096 bytes per field and contain no NUL";
    if (value.tags.find_first_of("\r\n") != std::string::npos)
        return "Tags must be a comma-separated single line";
    if (!value.rating.empty() &&
        (value.rating.size() != 1 || value.rating.front() < '0' || value.rating.front() > '5'))
        return "Rating must be empty or a whole number from 0 to 5";
    return {};
}

std::error_code set(const std::string& path, const char* key, const std::string& value) {
    if (value.empty()) {
        if (::removexattr(path.c_str(), key) == 0 || errno == ENODATA)
            return {};
    } else if (::setxattr(path.c_str(), key, value.data(), value.size(), 0) == 0)
        return {};
    return error();
}
}  // namespace

Annotations read_annotations(int fd) {
    Annotations result;
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        result.error = error().message();
        return result;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        result.error = "Annotations are unavailable for links and special files";
        return result;
    }
    const auto path = pinned(fd);
    const auto into = fields(result);
    std::array<char, kLimit> buffer{};
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        const auto length = ::getxattr(path.c_str(), kNames[i], buffer.data(), buffer.size());
        if (length >= 0)
            into[i]->assign(buffer.data(), static_cast<std::size_t>(length));
        else if (errno != ENODATA) {
            result.error = error().message();
            return result;
        }
    }
    return result;
}

Annotations read_annotations(const std::filesystem::path& path) {
    auto normalized = path;
    while (normalized != normalized.root_path() && !normalized.has_filename())
        normalized = normalized.parent_path();
    const int fd = ::open(normalized.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return {.tags = {}, .rating = {}, .comment = {}, .error = error().message()};
    auto result = read_annotations(fd);
    ::close(fd);
    return result;
}

std::string write_annotations(const PermissionTarget& target, const Annotations& before,
                              Annotations values, unsigned selected) {
    if (const auto invalid = validate(values); !invalid.empty())
        return invalid;
    if (!target.error.empty())
        return target.error;
    const int fd = ::open(target.path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return error().message();
    const auto save = [&]() -> std::string {
        struct stat info{};
        if (::fstat(fd, &info) != 0)
            return error().message();
        if (info.st_dev != target.device || info.st_ino != target.inode ||
            (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
            return "The item was replaced; reopen Properties";
        auto current = read_annotations(fd);
        if (!current.error.empty())
            return current.error;
        auto original = before;
        const auto previous = fields(original), latest = fields(current), wanted = fields(values);
        for (std::size_t i = 0; i < kNames.size(); ++i)
            if ((selected & (1U << i)) && *previous[i] != *latest[i])
                return "Annotations changed externally; reopen Properties";
        const auto path = pinned(fd);
        for (std::size_t i = 0; i < kNames.size(); ++i) {
            if (!(selected & (1U << i)) || *latest[i] == *wanted[i])
                continue;
            if (const auto failed = set(path, kNames[i], *wanted[i]); failed) {
                std::string message = failed.message();
                for (std::size_t j = 0; j < i; ++j)
                    if ((selected & (1U << j)) && set(path, kNames[j], *latest[j]))
                        message += "; restoring an earlier annotation failed";
                return message;
            }
        }
        return {};
    };
    const auto result = save();
    ::close(fd);
    return result;
}

std::error_code copy_annotations(int source, int destination) {
    std::array<char, kLimit> buffer{};
    for (const auto* key : kNames) {
        const auto size = ::fgetxattr(source, key, buffer.data(), buffer.size());
        if (size < 0) {
            if (errno == ENODATA || errno == ENOTSUP)
                continue;
            return error();
        }
        if (::fsetxattr(destination, key, buffer.data(), static_cast<std::size_t>(size), 0) != 0)
            return error();
    }
    return {};
}

}  // namespace rummage::fs
