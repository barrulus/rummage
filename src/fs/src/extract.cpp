#include "rummage/fs/extract.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <linux/openat2.h>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rummage/archive/archive.hpp"
#include "rummage/fs/archive_folder.hpp"

namespace rummage::fs {
namespace {

constexpr std::string_view kStaging = ".rummage-extract-";  // hidden, recognisable
constexpr int kMostNames = 1000;                            // "x 2" … tried before giving up

std::string errno_text(int error) {
    return std::error_code{error, std::generic_category()}.message();
}

// A descriptor closed when it goes.
class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

// openat2 with nothing allowed to lead out of dir: no "..", no absolute path, no symbolic link
// (one made earlier included), no /proc magic, no other mount. -1 with errno on a refusal.
int open_beneath(int dir, const char* name, std::uint64_t flags, std::uint64_t mode = 0) {
    open_how how{};
    how.flags = flags | O_CLOEXEC | O_NOFOLLOW;
    how.mode = mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    for (;;) {
        const long fd = ::syscall(SYS_openat2, dir, name, &how, sizeof(how));
        if (fd < 0 && errno == EINTR) {
            continue;
        }
        return static_cast<int>(fd);
    }
}

// "a/b/c" -> {"a/b", "c"}; "c" -> {"", "c"}.
std::pair<std::string, std::string> split(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

// Folders inside the staging folder, made as needed and opened one part at a time beneath it.
class Folders {
public:
    explicit Folders(int root) : root_{root} {}

    // The folder at path ("" for the root), made with its parents when missing; -1 with errno.
    int open(const std::string& path) {
        if (path.empty()) {
            return root_;
        }
        if (path == last_path_ && last_.get() >= 0) {
            return last_.get();
        }
        Fd current;
        int at = root_;
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t slash = std::min(path.find('/', start), path.size());
            const std::string part = path.substr(start, slash - start);
            start = slash + 1;
            if (::mkdirat(at, part.c_str(), 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            Fd next{open_beneath(at, part.c_str(), O_RDONLY | O_DIRECTORY)};
            if (next.get() < 0) {
                return -1;  // a file or a link where a folder is: never followed
            }
            current = std::move(next);
            at = current.get();
        }
        last_ = std::move(current);
        last_path_ = path;
        return last_.get();
    }

private:
    int root_;
    Fd last_;  // the last folder opened, kept: entries come folder by folder
    std::string last_path_;
};

// The archive's permission bits as rummage keeps them: no setuid, setgid or sticky; readable
// (folders also searchable and writable) by the owner.
mode_t file_mode(std::uint32_t mode) {
    const auto bits = static_cast<mode_t>(mode & 0777U);
    return (bits == 0 ? 0644 : bits) | S_IRUSR;
}

mode_t folder_mode(std::uint32_t mode) {
    const auto bits = static_cast<mode_t>(mode & 0777U);
    return (bits == 0 ? 0755 : bits) | S_IRWXU;
}

std::array<timespec, 2> times_of(std::int64_t mtime) {
    const timespec when{.tv_sec = static_cast<time_t>(mtime), .tv_nsec = 0};
    return {when, when};
}

// A symbolic link's target is kept when, followed from the link's folder inside what is
// extracted (top: the first part of the link's path), it stays there: relative, never above top.
bool link_stays_inside(const std::string& link_path, const std::string& target) {
    if (target.empty() || target.starts_with('/') || target.find('\0') != std::string::npos) {
        return false;
    }
    std::vector<std::string_view> parts;
    const auto [folder, name] = split(link_path);
    std::string_view rest = folder;
    while (!rest.empty()) {
        const std::size_t slash = std::min(rest.find('/'), rest.size());
        parts.push_back(rest.substr(0, slash));
        rest = slash < rest.size() ? rest.substr(slash + 1) : std::string_view{};
    }
    std::string_view walk = target;
    while (!walk.empty()) {
        const std::size_t slash = std::min(walk.find('/'), walk.size());
        const std::string_view part = walk.substr(0, slash);
        walk = slash < walk.size() ? walk.substr(slash + 1) : std::string_view{};
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (parts.size() <= 1) {
                return false;  // above the top item
            }
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    return true;
}

// One thing to make: its path under the staging folder and the archive's entry (null: a folder
// made up from the names under it).
struct Item {
    std::string out;
    std::string inner;  // its clean path in the archive
    const archive::Entry* entry{nullptr};
};

class Extractor {
public:
    Extractor(const TransferRequest& request, const core::CancelToken& token,
              const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks} {}

    TransferResult run() {
        if (!request_.extract_into.empty() &&
            (request_.extract_into == "." || request_.extract_into == ".." ||
             request_.extract_into.find('/') != std::string::npos ||
             request_.extract_into.find('\0') != std::string::npos)) {
            result_.refused = "Invalid extraction folder name";
            return std::move(result_);
        }
        const std::optional<ArchiveLocation> where = find_archive(request_.source);
        if (!where) {
            result_.refused = "Not inside an archive";
            return std::move(result_);
        }
        tree_ = archive_tree(where->archive, nullptr);
        if (tree_->failure() != archive::Failure::none && tree_->folder("") != nullptr &&
            tree_->folder("")->empty()) {
            result_.refused = "The archive cannot be read: " + tree_->detail();
            return std::move(result_);
        }
        if (find_archive(request_.destination)) {
            result_.refused = "Nothing inside an archive can be changed";
            return std::move(result_);
        }
        if (tree_->failure() != archive::Failure::none) {
            result_.errors.push_back(std::format("the archive is damaged: {}", tree_->detail()));
        }
        archive_file_ = Fd{::open(where->archive.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
        const std::filesystem::path pinned = "/proc/self/fd/" + std::to_string(archive_file_.get());
        if (archive_file_.get() < 0 || !tree_->unchanged(pinned)) {
            result_.refused = "Archive changed since it was listed; reopen it and try again";
            return std::move(result_);
        }
        gather(*where);
        if (!result_.refused.empty() || !check(pinned)) {
            return std::move(result_);
        }
        // The folder chosen is followed as a copy follows it (a link to another disk); what is
        // made under it never is.
        destination_ = Fd{::open(request_.destination.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (destination_.get() < 0) {
            result_.refused =
                std::format("{}: {}", request_.destination.string(), errno_text(errno));
            return std::move(result_);
        }
        if (!make_staging()) {
            return std::move(result_);
        }
        extract(pinned);
        if (!tree_->unchanged(pinned)) {
            result_.refused = "Archive changed while extracting; no files published";
        } else if (token_.cancelled()) {
            result_.cancelled = true;
        } else {
            publish();
        }
        std::error_code ignored;
        std::filesystem::remove_all(request_.destination / staging_name_, ignored);
        return std::move(result_);
    }

private:
    // What is asked for: every entry at or under each name, its path made relative to the
    // folder the names are in (and put inside extract_into).
    void gather(const ArchiveLocation& where) {
        const std::string prefix = where.inner.empty() ? std::string{} : where.inner + "/";
        std::unordered_set<std::string> selected;
        for (const std::string& name : request_.names) {
            if (name.empty() || name == "." || name == ".." ||
                name.find('/') != std::string::npos || name.find('\0') != std::string::npos ||
                !selected.insert(name).second) {
                result_.refused = "Invalid or repeated archive selection";
                return;
            }
        }
        std::unordered_set<std::string> found;
        // Scan the tree once, even when every item is selected.
        tree_->each_under(where.inner, [&](const std::string& path, const archive::Entry* entry) {
            if (!path.starts_with(prefix) || path.size() <= prefix.size())
                return;
            std::string out = path.substr(prefix.size());
            const std::string top = out.substr(0, out.find('/'));
            if (!selected.contains(top))
                return;
            found.insert(top);
            if (!request_.extract_into.empty())
                out.insert(0, request_.extract_into + '/');
            items_.push_back({.out = std::move(out), .inner = path, .entry = entry});
        });
        for (const auto& name : request_.names)
            if (!found.contains(name))
                result_.errors.push_back(name + ": not in the archive");
        tops_ = request_.extract_into.empty() ? request_.names
                                              : std::vector<std::string>{request_.extract_into};
        // Folders first (shallow before deep), then files in the archive's order (a tar.gz is
        // inflated once), then links (a hard link needs its file out first).
        const auto rank = [](const Item& item) {
            if (item.entry == nullptr || item.entry->type == archive::Type::directory) {
                return 0;
            }
            if (item.entry->type == archive::Type::file) {
                return 1;
            }
            return 2;
        };
        std::ranges::sort(items_, [&](const Item& a, const Item& b) {
            const int ra = rank(a);
            const int rb = rank(b);
            if (ra != rb) {
                return ra < rb;
            }
            if (ra == 0) {
                return std::ranges::count(a.out, '/') < std::ranges::count(b.out, '/') ||
                       (std::ranges::count(a.out, '/') == std::ranges::count(b.out, '/') &&
                        a.out < b.out);
            }
            if (ra == 1 && a.entry->locator != b.entry->locator) {
                return a.entry->locator < b.entry->locator;
            }
            return a.out < b.out;
        });
        for (const Item& item : items_) {
            if (item.entry != nullptr && item.entry->type == archive::Type::file &&
                item.entry->unreadable == archive::Unreadable::none) {
                if (item.entry->size > UINT64_MAX - progress_.bytes_total) {
                    result_.refused = "Archive expanded size exceeds the supported limit";
                    return;
                }
                progress_.bytes_total += item.entry->size;
                ++progress_.files_total;
            }
        }
    }

    // The free space, and an archive that unpacks to far more than it weighs.
    bool check(const std::filesystem::path& archive) {
        struct statvfs space{};
        if (::statvfs(request_.destination.c_str(), &space) == 0) {
            const std::uint64_t free = static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize;
            if (progress_.bytes_total > free) {
                result_.refused = std::format("Not enough space: {} needed, {} free",
                                              human_size(progress_.bytes_total), human_size(free));
                return false;
            }
        }
        std::error_code error;
        const std::uintmax_t packed = std::filesystem::file_size(archive, error);
        if (!error && progress_.bytes_total > kAskAbove &&
            progress_.bytes_total / std::max<std::uint64_t>(packed, 1) > kAskRatio) {
            Conflict question;
            question.name = archive.filename().string();
            question.destination = request_.destination;
            question.question = std::format(
                "It unpacks to {} from {}: {} times its size.", human_size(progress_.bytes_total),
                human_size(packed), progress_.bytes_total / std::max<std::uint64_t>(packed, 1));
            if (!callbacks_.resolve || callbacks_.resolve(question) != Resolution::replace) {
                result_.cancelled = true;
                return false;
            }
        }
        return true;
    }

    bool make_staging() {
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::string name = std::format("{}{:08x}", kStaging, random());
            if (::mkdirat(destination_.get(), name.c_str(), 0700) == 0) {
                staging_ =
                    Fd{open_beneath(destination_.get(), name.c_str(), O_RDONLY | O_DIRECTORY)};
                if (staging_.get() < 0) {
                    result_.refused = errno_text(errno);
                    ::unlinkat(destination_.get(), name.c_str(), AT_REMOVEDIR);
                    return false;
                }
                staging_name_ = std::move(name);
                return true;
            }
            if (errno != EEXIST) {
                result_.refused =
                    std::format("{}: {}", request_.destination.string(), errno_text(errno));
                return false;
            }
        }
        result_.refused = "No free name for the extraction's folder";
        return false;
    }

    void fail(const Item& item, std::string_view why) {
        result_.errors.push_back(std::format("{}: {}", item.out, why));
    }

    void extract(const std::filesystem::path& archive) {
        Folders folders{staging_.get()};
        // Shift+Esc stops an entry being written, however large: the engine is stopped with it.
        std::unique_ptr<archive::EntrySequence> sequence = archive::open_entry_sequence(
            archive, tree_->format(), [this] { return token_.cancelled(); });
        std::unordered_map<std::string, std::string> made;  // inner path -> out path, files
        std::vector<const Item*> folder_items;
        for (const Item& item : items_) {
            if (token_.cancelled()) {
                return;
            }
            const archive::Entry* entry = item.entry;
            if (entry == nullptr || entry->type == archive::Type::directory) {
                if (folders.open(item.out) < 0) {
                    fail(item, errno_text(errno));
                } else if (entry != nullptr) {
                    folder_items.push_back(&item);
                }
                continue;
            }
            const auto [parent, name] = split(item.out);
            const int dir = folders.open(parent);
            if (dir < 0) {
                fail(item, errno_text(errno));
                continue;
            }
            switch (entry->type) {
                case archive::Type::file:
                    if (write_file(item, dir, name, *sequence)) {
                        made.emplace(item.inner, item.out);
                    }
                    break;
                case archive::Type::symlink:
                    if (!link_stays_inside(item.out, entry->link)) {
                        fail(item, "a link leading outside, left out");
                    } else if (::symlinkat(entry->link.c_str(), dir, name.c_str()) != 0) {
                        fail(item, errno_text(errno));
                    }
                    break;
                case archive::Type::hardlink: {
                    const std::optional<std::string> target = clean_inner_path(entry->link);
                    const auto it = target ? made.find(*target) : made.end();
                    if (it == made.end()) {
                        fail(item, "a link to a file not extracted, left out");
                        break;
                    }
                    const auto [target_parent, target_name] = split(it->second);
                    const Fd from{::dup(folders.open(target_parent))};
                    const int to = folders.open(parent);
                    if (from.get() < 0 || to < 0 ||
                        ::linkat(from.get(), target_name.c_str(), to, name.c_str(), 0) != 0) {
                        fail(item, errno_text(errno));
                    }
                    break;
                }
                case archive::Type::other:
                case archive::Type::directory:
                    fail(item, "not a file, left out");
                    break;
            }
        }
        // The folders' permissions and times last, deepest first: writing inside changes them.
        for (const Item* const each : std::views::reverse(folder_items)) {
            const Item& item = *each;
            const Fd dir{open_beneath(staging_.get(), item.out.c_str(), O_RDONLY | O_DIRECTORY)};
            if (dir.get() >= 0) {
                ::fchmod(dir.get(), folder_mode(item.entry->mode));
                if (item.entry->mtime != 0) {
                    const std::array<timespec, 2> times = times_of(item.entry->mtime);
                    ::futimens(dir.get(), times.data());
                }
            }
        }
    }

    bool write_file(const Item& item, int dir, const std::string& name,
                    archive::EntrySequence& sequence) {
        const archive::Entry& entry = *item.entry;
        if (entry.unreadable == archive::Unreadable::encrypted) {
            fail(item, "encrypted, left out");
            return false;
        }
        if (entry.unreadable != archive::Unreadable::none) {
            fail(item, "compressed in a way not read yet, left out");
            return false;
        }
        progress_.current = item.out;
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
        const Fd file{open_beneath(dir, name.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600)};
        if (file.get() < 0) {
            fail(item, errno_text(errno));
            return false;
        }
        // Never more than the archive says: an entry that holds more is refused by the engine.
        const archive::Read read = sequence.read(entry, file.get(), entry.size);
        if (read.failure != archive::Failure::none || read.bytes != entry.size) {
            ::unlinkat(dir, name.c_str(), 0);
            fail(item, read.detail.empty() ? std::string_view{"it could not be read"}
                                           : std::string_view{read.detail});
            return false;
        }
        ::fchmod(file.get(), file_mode(entry.mode));
        if (entry.mtime != 0) {
            const std::array<timespec, 2> times = times_of(entry.mtime);
            ::futimens(file.get(), times.data());
        }
        progress_.bytes_done += read.bytes;
        ++progress_.files_done;
        result_.bytes_done += read.bytes;
        ++result_.files_done;
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
        return true;
    }

    // Each item made, renamed into the destination under its name or the first free "x 2";
    // never over anything.
    void publish() {
        for (const std::string& top : tops_) {
            struct stat info{};
            if (::fstatat(staging_.get(), top.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;  // nothing of it could be extracted
            }
            const bool folder = S_ISDIR(info.st_mode);
            std::string name = top;
            bool placed = false;
            for (int attempt = 0; attempt < kMostNames && !placed; ++attempt) {
                if (attempt > 0) {
                    name = free_name(request_.destination, top, folder, {});
                }
                if (::renameat2(staging_.get(), top.c_str(), destination_.get(), name.c_str(),
                                RENAME_NOREPLACE) == 0) {
                    placed = true;
                } else if (errno != EEXIST) {
                    result_.errors.push_back(std::format("{}: {}", top, errno_text(errno)));
                    break;
                }
            }
            if (placed) {
                result_.placed.push_back(name);
                // From where a copy would take it again (Ctrl+Shift+Z): the archive's folder.
                result_.items.push_back({.from = request_.extract_into.empty()
                                                     ? request_.source / top
                                                     : request_.source,
                                         .to = request_.destination / name});
            }
        }
    }

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::shared_ptr<const ArchiveTree> tree_;
    std::vector<Item> items_;
    std::vector<std::string> tops_;
    TransferProgress progress_;
    TransferResult result_;
    Fd archive_file_;
    Fd destination_;
    Fd staging_;
    std::string staging_name_;
};

}  // namespace

TransferResult run_extract(const TransferRequest& request, const core::CancelToken& token,
                           const TransferCallbacks& callbacks) {
    return Extractor{request, token, callbacks}.run();
}

std::filesystem::path opened_files_folder() {
    const char* cache = std::getenv("XDG_CACHE_HOME");  // NOLINT(concurrency-mt-unsafe): read only
    std::filesystem::path base;
    if (cache != nullptr && *cache == '/') {
        base = cache;
    } else if (const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
               home != nullptr && *home == '/') {
        base = std::filesystem::path{home} / ".cache";
    } else {
        return {};
    }
    return base / "rummage" / "opened";
}

ExtractedToOpen extract_to_open(const std::filesystem::path& source,
                                const std::vector<std::string>& names,
                                const core::CancelToken& token) {
    ExtractedToOpen out;
    const std::filesystem::path base = opened_files_folder();
    std::error_code error;
    if (base.empty() || (std::filesystem::create_directories(base, error), error)) {
        out.result.refused = "No folder to extract into: " +
                             (base.empty() ? std::string{"HOME is not set"} : error.message());
        return out;
    }
    ::chmod(base.c_str(), 0700);
    // Yesterday's: gone (a program still holding one keeps reading it).
    const auto day_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24};
    for (std::filesystem::directory_iterator it{base, error}, end; !error && it != end;
         it.increment(error)) {
        std::error_code ignored;
        if (std::filesystem::last_write_time(it->path(), ignored) < day_ago && !ignored) {
            std::filesystem::remove_all(it->path(), ignored);
        }
    }
    std::string pattern = (base / "XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        out.result.refused = "No folder to extract into: " + errno_text(errno);
        return out;
    }
    out.folder = pattern;
    TransferCallbacks callbacks;
    callbacks.resolve = [](const Conflict& /*conflict*/) { return Resolution::cancel; };
    out.result = run_extract({.kind = TransferKind::copy,
                              .source = source,
                              .names = names,
                              .destination = out.folder,
                              .force_copy = false},
                             token, callbacks);
    if (out.result.cancelled && out.result.placed.empty() && !token.cancelled()) {
        out.result.refused = "It unpacks to far more than the archive weighs: not opened";
    }
    return out;
}

}  // namespace rummage::fs
