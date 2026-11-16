#include "rummage/fs/transfer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/operations.hpp"

namespace rummage::fs {
namespace {

constexpr std::size_t kChunk = std::size_t{8} << 20U;  // bytes per copy_file_range call
// Upstream selected 128 KiB for the read/write fallback between file systems.
constexpr std::size_t kBuffer = std::size_t{128} << 10U;
constexpr std::string_view kTemporary = ".rummage-part-";  // hidden, recognisable
// Threads copying the files of a folder at once (TransferRequest::threads == 0), and how many
// files may wait for them: the walk stays a little ahead, never the whole tree in memory.
// Keep the upstream default of eight copiers; callers can override it for measurements.
constexpr std::size_t kCopiers = 8;
constexpr std::size_t kWaiting = 256;

std::error_code last_error() { return {errno, std::generic_category()}; }

// A file descriptor closed on every path.
class Fd {
public:
    explicit Fd(int fd = -1) : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

    // Closes now; the error of close() matters for written files.
    std::error_code close_now() {
        const int fd = std::exchange(fd_, -1);
        return fd >= 0 && close(fd) != 0 ? last_error() : std::error_code{};
    }

private:
    int fd_;
};

std::optional<struct stat> status_of(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return info;
}

bool taken(const std::filesystem::path& path) { return status_of(path).has_value(); }

// The names in directory, unsorted; empty when it cannot be read.
std::vector<std::string> names_in(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code ignored;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

// rename(2) that never replaces (with a check first where the file system lacks it).
std::error_code rename_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return last_error();
    }
    if (taken(to)) {
        return std::make_error_code(std::errc::file_exists);
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

std::error_code rename_over(const std::filesystem::path& from, const std::filesystem::path& to) {
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

std::error_code sync_directory(const std::filesystem::path& directory) {
    const Fd fd{open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    return fd.get() < 0 || fsync(fd.get()) != 0 ? last_error() : std::error_code{};
}

// "P1.ORF" -> "P1 2.ORF"; a folder or a name without extension: "Trip 2".
std::string numbered(const std::string& name, bool folder, int number) {
    const std::size_t dot = name.rfind('.');
    if (folder || dot == std::string::npos || dot == 0) {
        return std::format("{} {}", name, number);
    }
    return std::format("{} {}{}", name.substr(0, dot), number, name.substr(dot));
}

// "3.4 GB", as the status bar writes sizes (decimal units).
std::string human_size(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> units{"bytes", "kB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < units.size()) {
        value /= 1000.0;
        ++unit;
    }
    return unit == 0 ? i18n::trf("{} bytes", bytes)
                     : i18n::decimal(std::format("{:.1f} {}", value, units.at(unit)));
}

// One top-level item and the sidecars that travel with it.
struct Group {
    std::string name;
    struct stat info{};
    std::vector<std::string> sidecars;
    std::vector<std::optional<struct stat>> sidecar_info;
};

// What became of a group: placed under name (none: skipped, failed or cancelled).
struct Outcome {
    std::optional<std::string> name;
    bool complete{true};           // a published folder may contain only the files that arrived
    bool exclude_undo{false};      // an undo could remove data that was already there
    bool merged{false};            // a folder merged into the one there
    std::vector<PlacedItem> made;  // merged: what it put there that was not there
};

struct CopiedSource {
    std::filesystem::path path;
    struct stat info{};
    std::string where;
};

// A folder being copied: finished (permissions, dates, the source removed on a move) only once
// every file in it has been copied, bottom-up.
struct Building {
    std::filesystem::path source;
    std::filesystem::path made;  // its hidden name, or its own inside another being built
    struct stat info{};
    std::string where;
    Building* parent{nullptr};
    bool failed{false};  // something in it did not arrive (Engine::state_)
    // A move deletes these only after the hidden folder has its final name.
    std::vector<CopiedSource> copied{};
};

// Work for the copiers: a file inside a folder being built, or a loose file and its sidecars.
struct Job {
    std::function<bool()> work;  // false: it did not arrive (the error is already written)
    Building* folder{nullptr};   // marked incomplete then
};

class Engine {
public:
    Engine(const TransferRequest& request, const core::CancelToken& token,
           const TransferCallbacks& callbacks)
        : request_{request},
          token_{token},
          callbacks_{callbacks},
          threads_{request.threads == 0 ? kCopiers : request.threads} {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    ~Engine() { stop_copiers(); }

    TransferResult run();

private:
    TransferResult finished() {
        stop_copiers();
        result_.cancelled = cancelled_;
        return std::move(result_);
    }

    // Any thread.
    [[nodiscard]] bool cancelled() {
        if (token_.cancelled()) {
            cancelled_ = true;
        }
        return cancelled_;
    }

    // Progress and errors come from the walk and from the copiers: one at a time, and so are the
    // callbacks (TransferQueue's throttle is not thread-safe).
    void report() {
        const std::scoped_lock lock{state_};
        report_locked();
    }

    void report_locked() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    void add_bytes(std::uint64_t bytes) {
        const std::scoped_lock lock{state_};
        progress_.bytes_done += bytes;
        result_.bytes_done += bytes;
        report_locked();
    }

    void add_file() {
        const std::scoped_lock lock{state_};
        ++progress_.files_done;
        ++result_.files_done;
        report_locked();
    }

    void error(const std::string& where, const std::error_code& code) {
        const std::scoped_lock lock{state_};
        result_.errors.push_back(std::format("{}: {}", where, code.message()));
    }

    void error(const std::string& where, std::string_view what) {
        const std::scoped_lock lock{state_};
        result_.errors.push_back(std::format("{}: {}", where, what));
    }

    void fail(Building* folder) {
        if (folder != nullptr) {
            const std::scoped_lock lock{state_};
            folder->failed = true;
        }
    }

    // The copiers: started at the first file handed over, stopped by the destructor.
    void hand_over(Job job);
    void wait_for_copiers();  // until every file handed over is done
    void copier();
    void stop_copiers();
    // Inside the folder an outermost copy is building, once its files are done: each folder
    // finished, the deepest first.
    bool finish(std::deque<Building>& folders);
    void remove_sources(std::deque<Building>& folders);
    bool remove_source(const std::filesystem::path& source, const struct stat& info,
                       const std::string& where);

    void count(const std::filesystem::path& path);
    // Moves or copies one item to target; where names it in messages. false when it failed or
    // was cancelled (the source then stays). fresh: target's folder is one this transfer is
    // building under a hidden name, so nobody sees target before that folder is renamed, and a
    // cancelled copy removes it whole: items are written under their own names, no temporary.
    bool place(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::string& where, bool replace, bool fresh = false, bool* complete = nullptr,
               const struct stat* expected = nullptr);
    bool copy_file(const std::filesystem::path& source, const struct stat& info,
                   const std::filesystem::path& target, const std::string& where, bool replace,
                   bool fresh);
    bool copy_link(const std::filesystem::path& source, const struct stat& info,
                   const std::filesystem::path& target, const std::string& where, bool replace,
                   bool fresh);
    bool copy_folder(const std::filesystem::path& source, const struct stat& info,
                     const std::filesystem::path& target, const std::string& where, bool fresh,
                     bool* complete);
    // Walks a folder being built: links and folders made at once, files handed to the copiers.
    void fill(Building& folder, std::deque<Building>& folders);
    // The items named in source, each regular file with its sidecars (sidecars_of among
    // listing); a sidecar named along with its photo goes with it.
    std::vector<Group> group(const std::filesystem::path& source,
                             const std::vector<std::string>& names,
                             const std::vector<std::string>& listing, const std::string& where);
    // Places groups from source into target, asking about taken names (inside: within a merge,
    // where a folder meets a folder they merge without asking). Loose files go to the copiers,
    // each with its sidecars; done when they are.
    std::vector<Outcome> place_all(const std::filesystem::path& source,
                                   const std::filesystem::path& target,
                                   const std::vector<Group>& groups, const std::string& where,
                                   bool inside, bool same_folder);
    // A folder into the folder of the same name that is there: its items in turn, as place_all.
    // made gets what the merge put there that was not there before (what an undo takes away).
    bool merge(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::string& where, std::vector<PlacedItem>& made);
    bool copy_data(int from, int to, const std::string& where);
    // A free hidden name next to target for a temporary.
    std::filesystem::path temporary(const std::filesystem::path& target);

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::mutex state_;  // result_, progress_, Building::failed
    TransferResult result_;
    TransferProgress progress_;
    std::atomic<bool> cancelled_{false};
    bool copying_{false};  // data is copied (a copy, or a move between file systems)
    bool moving_{false};
    std::atomic<unsigned> temporaries_{0};  // copiers ask for temporaries too

    std::size_t threads_;
    std::mutex jobs_mutex_;
    std::condition_variable work_;  // a job or the end, for the copiers
    std::condition_variable room_;  // room in jobs_, or all done, for the walk
    std::deque<Job> jobs_;
    std::size_t busy_{0};
    bool stopping_{false};
    std::vector<std::thread> copiers_;
};

void Engine::hand_over(Job job) {
    std::unique_lock lock{jobs_mutex_};
    if (copiers_.empty()) {
        for (std::size_t i = 0; i < threads_; ++i) {
            copiers_.emplace_back([this] { copier(); });
        }
    }
    room_.wait(lock, [this] { return jobs_.size() < kWaiting; });
    jobs_.push_back(std::move(job));
    lock.unlock();
    work_.notify_one();
}

void Engine::wait_for_copiers() {
    std::unique_lock lock{jobs_mutex_};
    room_.wait(lock, [this] { return jobs_.empty() && busy_ == 0; });
}

void Engine::copier() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{jobs_mutex_};
            work_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++busy_;
        }
        room_.notify_all();
        const bool copied = !cancelled() && job.work();
        if (!copied) {
            fail(job.folder);
        }
        {
            const std::scoped_lock lock{jobs_mutex_};
            --busy_;
        }
        room_.notify_all();
    }
}

void Engine::stop_copiers() {
    {
        const std::scoped_lock lock{jobs_mutex_};
        stopping_ = true;
    }
    work_.notify_all();
    for (std::thread& thread : copiers_) {
        thread.join();
    }
    copiers_.clear();
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
void Engine::count(const std::filesystem::path& path) {
    const std::optional<struct stat> info = status_of(path);
    if (!info) {
        return;
    }
    if (S_ISDIR(info->st_mode)) {
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator{path, ignored}) {
            if (token_.cancelled()) {
                return;
            }
            count(entry.path());
        }
        return;
    }
    ++progress_.files_total;
    if (S_ISREG(info->st_mode)) {
        progress_.bytes_total += static_cast<std::uint64_t>(info->st_size);
    }
}

std::filesystem::path Engine::temporary(const std::filesystem::path& target) {
    const std::filesystem::path parent = target.parent_path();
    for (;;) {
        std::filesystem::path candidate =
            parent / std::format("{}{}-{}", kTemporary, getpid(), ++temporaries_);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

bool Engine::copy_data(int from, int to, const std::string& where) {
    bool fallback = false;
    std::vector<char> buffer;
    for (;;) {
        if (cancelled()) {
            return false;
        }
        ssize_t copied = 0;
        if (!fallback) {
            copied = copy_file_range(from, nullptr, to, nullptr, kChunk, 0);
            if (copied < 0 &&
                (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                fallback = true;  // not across these file systems: plain reads and writes
                continue;
            }
        } else {
            buffer.resize(kBuffer);
            copied = read(from, buffer.data(), buffer.size());
            for (ssize_t written = 0; copied > 0 && written < copied;) {
                const ssize_t now =
                    write(to, buffer.data() + written, static_cast<std::size_t>(copied - written));
                if (now < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    error(where, last_error());
                    return false;
                }
                written += now;
            }
        }
        if (copied < 0) {
            if (errno == EINTR) {
                continue;
            }
            error(where, last_error());
            return false;
        }
        if (copied == 0) {
            return true;
        }
        add_bytes(static_cast<std::uint64_t>(copied));
    }
}

bool Engine::copy_file(const std::filesystem::path& source, const struct stat& info,
                       const std::filesystem::path& target, const std::string& where, bool replace,
                       bool fresh) {
    // Never through a link, never blocked on a pipe: the file must still be the one lstat saw.
    const Fd from{open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (from.get() < 0) {
        if (errno == ELOOP) {
            error(where, i18n::tr("changed while being copied"));  // a link now
        } else {
            error(where, last_error());
        }
        return false;
    }
    struct stat opened{};
    if (fstat(from.get(), &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != info.st_dev || opened.st_ino != info.st_ino) {
        error(where, i18n::tr("changed while being copied"));
        return false;
    }
    const std::filesystem::path part = fresh ? target : temporary(target);
    Fd to{open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (to.get() < 0) {
        error(where, last_error());
        return false;
    }
    const auto give_up = [&part] { unlink(part.c_str()); };
    if (!copy_data(from.get(), to.get(), where)) {
        give_up();
        return false;
    }
    const std::array<timespec, 2> times{info.st_atim, info.st_mtim};
    fchmod(to.get(), info.st_mode & 07777);
    futimens(to.get(), times.data());
    // Between file systems the original is deleted next: the copy must be on the disk first.
    if (moving_ && fsync(to.get()) != 0) {
        error(where, last_error());
        give_up();
        return false;
    }
    if (const std::error_code closed = to.close_now()) {
        error(where, closed);
        give_up();
        return false;
    }
    if (!fresh) {
        if (const std::error_code placed =
                replace ? rename_over(part, target) : rename_new(part, target)) {
            error(where, placed);
            give_up();
            return false;
        }
    }
    if (moving_ && !fresh) {
        if (const std::error_code synced = sync_directory(target.parent_path())) {
            error(where, synced);
        } else {
            remove_source(source, info, where);
        }
    }
    add_file();
    return true;
}

bool Engine::copy_link(const std::filesystem::path& source, const struct stat& info,
                       const std::filesystem::path& target, const std::string& where, bool replace,
                       bool fresh) {
    std::string link(static_cast<std::size_t>(std::max<off_t>(info.st_size, 0)) + 1, '\0');
    const ssize_t length = readlink(source.c_str(), link.data(), link.size());
    if (length < 0) {
        error(where, last_error());
        return false;
    }
    link.resize(static_cast<std::size_t>(length));
    const std::filesystem::path part = fresh ? target : temporary(target);
    if (symlink(link.c_str(), part.c_str()) != 0) {
        error(where, last_error());
        return false;
    }
    const std::array<timespec, 2> times{info.st_atim, info.st_mtim};
    utimensat(AT_FDCWD, part.c_str(), times.data(), AT_SYMLINK_NOFOLLOW);
    if (!fresh) {
        if (const std::error_code placed =
                replace ? rename_over(part, target) : rename_new(part, target)) {
            error(where, placed);
            unlink(part.c_str());
            return false;
        }
    }
    if (moving_ && !fresh) {
        if (const std::error_code synced = sync_directory(target.parent_path())) {
            error(where, synced);
        } else {
            remove_source(source, info, where);
        }
    }
    add_file();
    return true;
}

bool Engine::copy_folder(const std::filesystem::path& source, const struct stat& info,
                         const std::filesystem::path& target, const std::string& where, bool fresh,
                         bool* completed) {
    if (fresh) {
        // fill() makes folders inside one being built itself; nothing else asks for one.
        error(where, i18n::tr("a folder inside a copy: not expected here"));
        return false;
    }
    // Built under a hidden name and renamed when complete: a cancelled copy leaves no half folder.
    // What is inside goes under its own names at once (nobody sees it before the rename), its
    // files copied by several threads; the folders are finished once every file has arrived.
    const std::filesystem::path part = temporary(target);
    if (mkdir(part.c_str(), 0700) != 0) {
        error(where, last_error());
        return false;
    }
    std::deque<Building> folders;  // references stay valid as it grows: the copiers hold some
    Building& top = folders.emplace_back(
        Building{.source = source, .made = part, .info = info, .where = where, .parent = nullptr});
    const std::size_t files_before = [&] {
        const std::scoped_lock lock{state_};
        return result_.files_done;
    }();
    fill(top, folders);
    wait_for_copiers();
    const auto give_up = [this, &part, files_before] {
        std::error_code ignored;
        std::filesystem::remove_all(part, ignored);
        const std::scoped_lock lock{state_};
        result_.files_done = files_before;
        progress_.files_done = files_before;
    };
    if (cancelled()) {
        give_up();
        return false;
    }
    const bool complete = finish(folders);
    if (completed != nullptr) {
        *completed = complete;
    }
    if (const std::error_code placed = rename_new(part, target)) {
        error(where, placed);
        give_up();
        return false;
    }
    if (moving_) {
        bool durable = true;
        // File fsync alone does not make their directory entries durable. Publish the name,
        // then flush every built directory and its parent before deleting any original.
        for (const Building& building : std::views::reverse(folders)) {
            const std::filesystem::path published = target / building.made.lexically_relative(part);
            if (const std::error_code synced = sync_directory(published)) {
                error(building.where, synced);
                durable = false;
            }
        }
        if (const std::error_code synced = sync_directory(target.parent_path())) {
            error(where, synced);
            durable = false;
        }
        if (durable) {
            remove_sources(folders);
        } else if (completed != nullptr) {
            *completed = false;
        }
    }
    return true;  // the published folder belongs in placed/items, even if some files failed
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
void Engine::fill(Building& folder, std::deque<Building>& folders) {
    std::error_code listed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{folder.source, listed}) {
        if (cancelled()) {
            return;
        }
        const std::string name = entry.path().filename().string();
        const std::filesystem::path target = folder.made / name;
        std::string where = std::format("{}/{}", folder.where, name);
        const std::optional<struct stat> info = status_of(entry.path());
        if (!info) {
            error(where, last_error());
            fail(&folder);
            continue;
        }
        if (S_ISREG(info->st_mode)) {
            hand_over({.work =
                           [this, source = entry.path(), target, info = *info,
                            where = std::move(where), building = &folder] {
                               if (!copy_file(source, info, target, where, false, true)) {
                                   return false;
                               }
                               if (moving_) {
                                   const std::scoped_lock lock{state_};
                                   building->copied.push_back(
                                       {.path = source, .info = info, .where = where});
                               }
                               return true;
                           },
                       .folder = &folder});
        } else if (S_ISDIR(info->st_mode)) {
            if (mkdir(target.c_str(), 0700) != 0) {
                error(where, last_error());
                fail(&folder);
                continue;
            }
            Building& inner = folders.emplace_back(Building{.source = entry.path(),
                                                            .made = target,
                                                            .info = *info,
                                                            .where = std::move(where),
                                                            .parent = &folder});
            fill(inner, folders);
        } else if (S_ISLNK(info->st_mode)) {
            if (!copy_link(entry.path(), *info, target, where, false, true)) {
                fail(&folder);
            } else if (moving_) {
                const std::scoped_lock lock{state_};
                folder.copied.push_back({.path = entry.path(), .info = *info, .where = where});
            }
        } else {
            error(where, "a special file (device, socket, pipe): not copied");
            fail(&folder);
        }
    }
    if (listed) {
        error(folder.where, listed);
        fail(&folder);
    }
}

bool Engine::finish(std::deque<Building>& folders) {
    // folders is in walking order, every folder before those inside it: backwards, the deepest
    // come first, and a folder's date is set after nothing more is made in it.
    for (Building& building : std::views::reverse(folders)) {
        bool failed = false;
        {
            const std::scoped_lock lock{state_};
            failed = building.failed;
            if (failed && building.parent != nullptr) {
                building.parent->failed = true;  // incomplete inside: so is the folder around it
            }
        }
        chmod(building.made.c_str(), building.info.st_mode & 07777);
        const std::array<timespec, 2> times{building.info.st_atim, building.info.st_mtim};
        utimensat(AT_FDCWD, building.made.c_str(), times.data(), 0);
    }
    const std::scoped_lock lock{state_};
    return !folders.front().failed;
}

bool Engine::remove_source(const std::filesystem::path& source, const struct stat& info,
                           const std::string& where) {
    const std::optional<struct stat> now = status_of(source);
    if (!now || now->st_dev != info.st_dev || now->st_ino != info.st_ino ||
        (now->st_mode & S_IFMT) != (info.st_mode & S_IFMT) || now->st_size != info.st_size ||
        now->st_mtim.tv_sec != info.st_mtim.tv_sec ||
        now->st_mtim.tv_nsec != info.st_mtim.tv_nsec) {
        error(where, i18n::tr("changed while being moved"));
        return false;
    }
    if (unlink(source.c_str()) != 0) {
        error(where, last_error());
        return false;
    }
    return true;
}

void Engine::remove_sources(std::deque<Building>& folders) {
    // The destination is now visible. Completed files may safely leave the source, including
    // those in a partially successful move; cancellation before publication leaves them all.
    for (Building& building : std::views::reverse(folders)) {
        for (const CopiedSource& copied : building.copied) {
            if (!remove_source(copied.path, copied.info, copied.where)) {
                building.failed = true;
            }
        }
        if (!building.failed) {
            const std::optional<struct stat> now = status_of(building.source);
            if (!now || !S_ISDIR(now->st_mode) || now->st_dev != building.info.st_dev ||
                now->st_ino != building.info.st_ino) {
                error(building.where, i18n::tr("changed while being moved"));
                building.failed = true;
            } else if (rmdir(building.source.c_str()) != 0) {
                error(building.where, last_error());
                building.failed = true;
            }
        }
        if (building.failed && building.parent != nullptr) {
            building.parent->failed = true;
        }
    }
}

std::vector<Group> Engine::group(const std::filesystem::path& source,
                                 const std::vector<std::string>& names,
                                 const std::vector<std::string>& listing,
                                 const std::string& where) {
    std::vector<Group> groups;
    for (const std::string& name : names) {
        Group group{.name = name, .info = {}, .sidecars = {}, .sidecar_info = {}};
        const std::optional<struct stat> info = status_of(source / name);
        if (!info) {
            error(where.empty() ? name : std::format("{}/{}", where, name), last_error());
            continue;
        }
        group.info = *info;
        if (S_ISREG(info->st_mode)) {
            for (const std::string& sidecar : sidecars_of(listing, name)) {
                const std::optional<struct stat> sidecar_info = status_of(source / sidecar);
                // A similarly named folder is independent, even when explicitly selected.
                // Loose photo jobs must never recurse into a folder from a copier thread.
                if (sidecar_info && !S_ISREG(sidecar_info->st_mode) &&
                    !S_ISLNK(sidecar_info->st_mode)) {
                    continue;
                }
                group.sidecars.push_back(sidecar);
                group.sidecar_info.push_back(sidecar_info);
            }
        }
        groups.push_back(std::move(group));
    }
    std::erase_if(groups, [&groups](const Group& item) {
        return std::ranges::any_of(groups, [&item](const Group& other) {
            return std::ranges::find(other.sidecars, item.name) != other.sidecars.end();
        });
    });
    return groups;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
std::vector<Outcome> Engine::place_all(const std::filesystem::path& source,
                                       const std::filesystem::path& target,
                                       const std::vector<Group>& groups, const std::string& where,
                                       bool inside, bool same_folder) {
    // Loose files are copied by the copiers, each with its sidecars, while the next ones are
    // looked at: the names they will take are claimed here, before they exist on the disk.
    std::set<std::string> claimed;
    const auto taken_here = [&](const std::string& name) {
        return claimed.contains(name) || taken(target / name);
    };
    const std::vector<std::string> target_listing = names_in(target);
    std::vector<Outcome> outcomes(groups.size());  // state_
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const Group& group = groups[index];
        if (cancelled()) {
            break;
        }
        const bool folder = S_ISDIR(group.info.st_mode);
        const std::string label =
            where.empty() ? group.name : std::format("{}/{}", where, group.name);
        std::string desired_name = group.name;
        if (!inside && !request_.destination_names.empty()) {
            const auto original = std::ranges::find(request_.names, group.name);
            desired_name = request_.destination_names[static_cast<std::size_t>(
                std::distance(request_.names.begin(), original))];
        }
        std::string name = desired_name;
        const auto sidecar_target = [&group](const std::string& sidecar, const std::string& as) {
            return sidecar_name(sidecar, group.name, as);
        };
        bool conflict = taken_here(name);
        for (const std::string& sidecar : group.sidecars) {
            conflict = conflict || taken_here(sidecar_target(sidecar, name));
        }
        bool replace = false;
        bool merging = false;
        if (conflict) {
            const std::optional<struct stat> there = status_of(target / name);
            const bool there_folder = there && S_ISDIR(there->st_mode);
            Resolution resolution = Resolution::keep_both;  // a copy into its own folder
            if (inside && folder && there_folder) {
                resolution = Resolution::replace;  // the merge asked for goes on inside
            } else if (!same_folder) {
                resolution = callbacks_.resolve
                                 ? callbacks_.resolve({.name = name,
                                                       .destination = target,
                                                       .directory = folder,
                                                       .target_directory = there_folder})
                                 : Resolution::skip;
            }
            if (resolution == Resolution::cancel) {
                cancelled_ = true;
                break;
            }
            if (resolution == Resolution::skip) {
                continue;
            }
            if (resolution == Resolution::keep_both) {
                for (int number = 2;; ++number) {
                    name = numbered(desired_name, folder, number);
                    bool free = !taken_here(name);
                    for (const std::string& sidecar : group.sidecars) {
                        free = free && !taken_here(sidecar_target(sidecar, name));
                    }
                    if (free) {
                        break;
                    }
                }
            } else {
                replace = true;
                merging = folder && there_folder;
            }
        }
        claimed.insert(name);
        for (const std::string& sidecar : group.sidecars) {
            claimed.insert(sidecar_target(sidecar, name));
        }
        {
            const std::scoped_lock lock{state_};
            progress_.current = name;
            report_locked();
        }
        if (merging) {
            // Here, once the loose files before it are done, so errors come in order.
            wait_for_copiers();
            Outcome& outcome = outcomes[index];
            const std::optional<struct stat> now = status_of(source / group.name);
            if (!now || now->st_dev != group.info.st_dev || now->st_ino != group.info.st_ino ||
                !S_ISDIR(now->st_mode)) {
                error(label, i18n::tr("changed while being copied"));
                continue;
            }
            outcome.merged = true;
            if (merge(source / group.name, target / name, label, outcome.made)) {
                outcome.name = name;
            }
            continue;
        }
        // The photo first, its sidecars only once it arrived.
        // NOLINTNEXTLINE(misc-no-recursion): place merges, a merge places: a folder's depth
        const bool destination_sidecars =
            S_ISREG(group.info.st_mode) && !sidecars_of(target_listing, name).empty();
        auto item = [this, &source, &target, &group, &outcomes, sidecar_target, index, name, label,
                     where, replace, destination_sidecars] {
            const bool exclude_undo = (replace && taken(target / name)) || destination_sidecars;
            bool complete = true;
            if (!place(source / group.name, target / name, label, replace, false, &complete,
                       &group.info)) {
                return false;  // the photo did not arrive: its sidecars stay with it
            }
            complete = complete && (!moving_ || !taken(source / group.name));
            {
                const std::scoped_lock lock{state_};
                outcomes[index].name = name;
                outcomes[index].complete = complete;
                outcomes[index].exclude_undo = exclude_undo;
                result_.undo_excluded += exclude_undo ? 1 : 0;
            }
            for (std::size_t sidecar_index = 0; sidecar_index < group.sidecars.size();
                 ++sidecar_index) {
                const std::string& sidecar = group.sidecars[sidecar_index];
                const std::string sidecar_label =
                    where.empty() ? sidecar : std::format("{}/{}", where, sidecar);
                if (!group.sidecar_info[sidecar_index]) {
                    error(sidecar_label, i18n::tr("changed while being copied"));
                    const std::scoped_lock lock{state_};
                    outcomes[index].complete = false;
                    continue;
                }
                if (!place(source / sidecar, target / sidecar_target(sidecar, name), sidecar_label,
                           replace, false, nullptr, &*group.sidecar_info[sidecar_index])) {
                    const std::scoped_lock lock{state_};
                    outcomes[index].complete = false;
                    if (cancelled()) {
                        break;
                    }
                } else if (moving_ && taken(source / sidecar)) {
                    const std::scoped_lock lock{state_};
                    outcomes[index].complete = false;
                }
            }
            return true;
        };
        if (copying_ && S_ISREG(group.info.st_mode)) {
            hand_over({.work = std::move(item), .folder = nullptr});
        } else {
            // A folder (its own files go to the copiers), a link, a rename: here, once the
            // loose files before it are done, so errors come in order.
            wait_for_copiers();
            item();
        }
    }
    wait_for_copiers();
    return outcomes;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
bool Engine::merge(const std::filesystem::path& source, const std::filesystem::path& target,
                   const std::string& where, std::vector<PlacedItem>& made) {
    std::vector<std::string> names;
    std::error_code listed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{source, listed}) {
        names.push_back(entry.path().filename().string());
    }
    if (listed) {
        error(where, listed);
        return false;
    }
    std::ranges::sort(names);
    const std::vector<Group> groups = group(source, names, names, where);
    const std::vector<Outcome> outcomes = place_all(source, target, groups, where, true, false);
    bool complete = groups.size() == names.size() - [&] {
        std::size_t sidecars = 0;
        for (const Group& g : groups) {
            sidecars += g.sidecars.size();
        }
        return sidecars;
    }();
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const Outcome& outcome = outcomes[index];
        complete = complete && outcome.name.has_value() && outcome.complete;
        if (outcome.merged) {
            made.insert(made.end(), outcome.made.begin(), outcome.made.end());
        } else if (outcome.name && !outcome.exclude_undo) {
            made.push_back({.from = source / groups[index].name, .to = target / *outcome.name});
        }
    }
    if (moving_ && complete && rmdir(source.c_str()) != 0) {
        error(where, last_error());
    }
    return complete;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
bool Engine::place(const std::filesystem::path& source, const std::filesystem::path& target,
                   const std::string& where, bool replace, bool fresh, bool* complete,
                   const struct stat* expected) {
    if (cancelled()) {
        return false;
    }
    const std::optional<struct stat> info = status_of(source);
    if (!info) {
        error(where, last_error());
        return false;
    }
    if (expected != nullptr &&
        (info->st_dev != expected->st_dev || info->st_ino != expected->st_ino ||
         (info->st_mode & S_IFMT) != (expected->st_mode & S_IFMT))) {
        error(where, i18n::tr("changed while being copied"));
        return false;
    }
    const std::optional<struct stat> existing = replace ? status_of(target) : std::nullopt;
    const bool folder = S_ISDIR(info->st_mode);
    if (existing && folder != S_ISDIR(existing->st_mode)) {
        error(where,
              std::string{
                  folder ? i18n::tr("a file has its name; a folder does not replace a file")
                         : i18n::tr("a folder has its name; a file does not replace a folder")});
        return false;
    }
    if (existing && folder) {
        std::vector<PlacedItem> made;  // place_all merges itself and keeps these
        return merge(source, target, where, made);
    }
    if (!copying_) {
        // A move on one file system: a rename, at once.
        const std::error_code moved =
            replace ? rename_over(source, target) : rename_new(source, target);
        if (!moved) {
            add_file();
            return true;
        }
        if (moved != std::errc::cross_device_link) {
            error(where, moved);
            return false;
        }
        copying_ = true;  // a mount point below the source: copy from here on
    }
    if (folder) {
        return copy_folder(source, *info, target, where, fresh, complete);
    }
    if (S_ISREG(info->st_mode)) {
        return copy_file(source, *info, target, where, replace, fresh);
    }
    if (S_ISLNK(info->st_mode)) {
        return copy_link(source, *info, target, where, replace, fresh);
    }
    error(where, std::string{i18n::tr("a special file (device, socket, pipe): not copied")});
    return false;
}

TransferResult Engine::run() {
    const TransferRequest& r = request_;
    if (!r.destination_names.empty() &&
        (r.destination_names.size() != r.names.size() ||
         std::ranges::any_of(r.destination_names, [](const std::string& name) {
             return name.empty() || name == "." || name == ".." ||
                    std::filesystem::path{name}.filename() != std::filesystem::path{name};
         }))) {
        result_.refused = i18n::tr("Invalid destination names");
        return finished();
    }
    moving_ = r.kind == TransferKind::move;
    const std::optional<struct stat> from = status_of(r.source);
    std::optional<struct stat> to = status_of(r.destination);
    if (!to && r.make_destination) {
        std::error_code made;
        std::filesystem::create_directories(r.destination, made);
        to = status_of(r.destination);
    }
    if (!to || !S_ISDIR(to->st_mode)) {
        result_.refused = i18n::trf("{} is not a folder", r.destination.string());
        return finished();
    }
    std::error_code ignored;
    const std::filesystem::path source_dir = std::filesystem::weakly_canonical(r.source, ignored);
    const std::filesystem::path target_dir =
        std::filesystem::weakly_canonical(r.destination, ignored);
    const bool same_folder = source_dir == target_dir;
    if (moving_ && same_folder && (r.destination_names.empty() || r.destination_names == r.names)) {
        result_.placed = r.names;  // already there
        return finished();
    }
    copying_ = !moving_ || r.force_copy || !from || from->st_dev != to->st_dev;

    const std::vector<Group> groups = group(r.source, r.names, names_in(r.source), "");

    for (const Group& group : groups) {
        if (!S_ISDIR(group.info.st_mode)) {
            continue;
        }
        const std::filesystem::path folder = source_dir / group.name;
        const auto [end, rest] = std::ranges::mismatch(folder, target_dir);
        if (end == folder.end()) {
            result_.refused = moving_ ? i18n::trf("Cannot move {} into itself", group.name)
                                      : i18n::trf("Cannot copy {} into itself", group.name);
            return finished();
        }
    }

    progress_.preparing = true;
    report();
    for (const Group& group : groups) {
        if (copying_) {
            count(r.source / group.name);
            for (const std::string& sidecar : group.sidecars) {
                count(r.source / sidecar);
            }
        } else {
            progress_.files_total += 1 + group.sidecars.size();
        }
    }
    progress_.preparing = false;
    if (cancelled()) {
        return finished();
    }
    if (copying_) {
        struct statvfs space{};
        if (statvfs(r.destination.c_str(), &space) == 0) {
            const std::uint64_t free = static_cast<std::uint64_t>(space.f_bavail) *
                                       static_cast<std::uint64_t>(space.f_frsize);
            if (progress_.bytes_total > free) {
                result_.refused = i18n::trf("Not enough space: {} needed, {} free",
                                            human_size(progress_.bytes_total), human_size(free));
                return finished();
            }
        }
    }
    report();

    const std::vector<Outcome> outcomes =
        place_all(r.source, r.destination, groups, "", false, same_folder && !moving_);
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const Outcome& outcome = outcomes[index];
        if (outcome.name) {
            result_.placed.push_back(*outcome.name);
        }
        if (outcome.merged) {
            result_.items.insert(result_.items.end(), outcome.made.begin(), outcome.made.end());
        } else if (outcome.name && !outcome.exclude_undo) {
            result_.items.push_back(
                {.from = r.source / groups[index].name, .to = r.destination / *outcome.name});
        }
    }
    {
        const std::scoped_lock lock{state_};
        progress_.current.clear();
        report_locked();
    }
    return finished();
}

}  // namespace

std::string free_name(const std::filesystem::path& directory, const std::string& name, bool folder,
                      const std::vector<std::string>& sidecars) {
    for (int number = 2;; ++number) {
        std::string candidate = numbered(name, folder, number);
        bool free = !taken(directory / candidate);
        for (const std::string& sidecar : sidecars) {
            free = free && !taken(directory / sidecar_name(sidecar, name, candidate));
        }
        if (free) {
            return candidate;
        }
    }
}

TransferResult run_transfer(const TransferRequest& request, const core::CancelToken& token,
                            const TransferCallbacks& callbacks) {
    return Engine{request, token, callbacks}.run();
}

}  // namespace rummage::fs
