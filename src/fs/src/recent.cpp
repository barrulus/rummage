#include "rummage/fs/recent.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rummage/core/file.hpp"

namespace rummage::fs {
namespace {
constexpr std::size_t kLimit = 200, kBytes = 1024 * 1024;

struct Lock {
    int fd{-1};

    explicit Lock(const std::filesystem::path& directory) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
            return;
        fd = ::open((directory / "recent.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0 && ::flock(fd, LOCK_EX) != 0) {
            ::close(fd);
            fd = -1;
        }
    }

    ~Lock() {
        if (fd >= 0)
            ::close(fd);
    }
};

std::vector<RecentFile> read(const std::filesystem::path& path) {
    std::vector<RecentFile> result;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return result;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > static_cast<off_t>(kBytes)) {
        ::close(fd);
        return result;
    }
    std::string text;
    std::array<char, 4096> buffer{};
    while (text.size() < kBytes) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text.append(buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    std::string_view rest = text;
    while (!rest.empty() && result.size() < kLimit) {
        const auto end = rest.find('\n');
        const auto line = rest.substr(0, end);
        rest.remove_prefix(std::min(rest.size(), line.size() + 1));
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos)
            continue;
        std::int64_t time{};
        const auto [ptr, err] = std::from_chars(line.data(), line.data() + tab, time);
        const auto file = core::path_from_uri(line.substr(tab + 1));
        if (err == std::errc{} && ptr == line.data() + tab && time > 0 && file &&
            std::ranges::none_of(result, [&](const auto& item) { return item.path == *file; }))
            result.push_back({*file, time});
    }
    return result;
}

std::string save(const std::filesystem::path& path, const std::vector<RecentFile>& files) {
    std::string text;
    for (const auto& item : files)
        text += std::to_string(item.opened) + "\t" + core::file_uri(item.path) + "\n";
    std::string temporary = (path.parent_path() / ".recent-XXXXXX").string();
    const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0)
        return std::error_code{errno, std::generic_category()}.message();
    std::error_code error;
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(fd, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = {errno ? errno : EIO, std::generic_category()};
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (!error && ::fsync(fd) != 0)
        error = {errno, std::generic_category()};
    if (::close(fd) != 0 && !error)
        error = {errno, std::generic_category()};
    if (!error && ::rename(temporary.c_str(), path.c_str()) != 0)
        error = {errno, std::generic_category()};
    if (error)
        ::unlink(temporary.c_str());
    return error ? error.message() : std::string{};
}
}  // namespace

RecentFiles::RecentFiles(std::filesystem::path directory) : directory_{std::move(directory)} {}

std::filesystem::path RecentFiles::default_directory() {
    if (const char* state = ::getenv("XDG_STATE_HOME"); state && *state == '/')
        return std::filesystem::path{state} / "rummage";
    if (const char* home = ::getenv("HOME"); home && *home == '/')
        return std::filesystem::path{home} / ".local/state/rummage";
    return {};
}

bool RecentFiles::enabled() const {
    std::error_code ec;
    return !directory_.empty() && !std::filesystem::exists(directory_ / "recent-disabled", ec) &&
           !ec;
}

std::vector<RecentFile> RecentFiles::load() const {
    if (!enabled())
        return {};
    auto files = read(directory_ / "recent-files");
    std::erase_if(files, [](const auto& file) {
        std::error_code ec;
        return !std::filesystem::is_regular_file(file.path, ec) ||
               ::access(file.path.c_str(), R_OK) != 0;
    });
    return files;
}

std::string RecentFiles::record(const std::vector<std::filesystem::path>& paths) const {
    if (directory_.empty())
        return "History directory unavailable";
    Lock lock{directory_};
    if (lock.fd < 0)
        return "History lock unavailable";
    if (!enabled())
        return {};
    auto files = read(directory_ / "recent-files");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto& path : paths) {
        std::error_code ec;
        if (!path.is_absolute() || !std::filesystem::is_regular_file(path, ec))
            continue;
        std::erase_if(files, [&](const auto& file) { return file.path == path; });
        files.insert(files.begin(), {path, now});
    }
    if (files.size() > kLimit)
        files.resize(kLimit);
    return save(directory_ / "recent-files", files);
}

std::string RecentFiles::clear() const {
    if (directory_.empty())
        return "History directory unavailable";
    Lock lock{directory_};
    return lock.fd < 0 ? "History lock unavailable" : save(directory_ / "recent-files", {});
}

std::string RecentFiles::set_enabled(bool value) const {
    if (directory_.empty())
        return "History directory unavailable";
    Lock lock{directory_};
    if (lock.fd < 0)
        return "History lock unavailable";
    std::error_code ec;
    if (value)
        std::filesystem::remove(directory_ / "recent-disabled", ec);
    else
        ec = core::write_file_atomically(directory_ / "recent-disabled", "disabled\n");
    if (ec)
        return ec.message();
    return value ? std::string{} : save(directory_ / "recent-files", {});
}
}  // namespace rummage::fs
