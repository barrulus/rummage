#include "rummage/git/decorator.hpp"

#include <algorithm>
#include <sys/wait.h>
#include <utility>

#include "repo.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/log.hpp"
#include "rummage/platform/program_output.hpp"

namespace rummage::git {
namespace {

using views::Badge;
using Tone = views::Badge::Tone;

// Changes keep coming (a build): status is read at the latest this long after the first.
constexpr std::chrono::milliseconds kMostDelay{2000};
// Folders remembered at once; more and the memory starts over.
constexpr std::size_t kPlaces = 4096;

// How much a folder's summary badge says about what is inside it: the strongest wins.
int weight(const Badge& badge) {
    if (badge.tone == Tone::removed) {
        return 3;
    }
    return badge.tone == Tone::added ? 1 : 2;
}

std::string_view change_name(char change) {
    switch (change) {
        case 'M':
            return i18n::tr("Modified");
        case 'T':
            return i18n::tr("Type changed");
        case 'A':
            return i18n::tr("Added");
        case 'D':
            return i18n::tr("Deleted");
        case 'R':
            return i18n::tr("Renamed");
        case 'C':
            return i18n::tr("Copied");
        default:
            return {};
    }
}

// A trash's own folders (freedesktop: $XDG_DATA_HOME/Trash/files and info, .Trash and
// .Trash-$UID at a mount's top), even opened by path: git status never shows in them.
bool in_a_trash(const std::filesystem::path& folder) {
    std::string_view previous;
    for (const std::filesystem::path& part : folder) {
        const std::string_view name{part.native()};
        if (name == ".Trash" || name.starts_with(".Trash-") ||
            (previous == "Trash" && (name == "files" || name == "info"))) {
            return true;
        }
        previous = name;
    }
    return false;
}

}  // namespace

std::optional<Badge> badge_of(const FileStatus& file) {
    switch (file.kind) {
        case FileStatus::Kind::conflict:
            return Badge{
                .text = "!", .tone = Tone::removed, .strong = true, .faded = false, .tint = true};
        case FileStatus::Kind::untracked:
            return Badge{
                .text = "U", .tone = Tone::added, .strong = false, .faded = false, .tint = true};
        case FileStatus::Kind::ignored:
            return Badge{.text = {}, .tone = Tone::muted, .strong = false, .faded = true};
        case FileStatus::Kind::tracked:
            break;
    }
    const char change = file.unstaged != '.' ? file.unstaged : file.staged;
    Badge badge{.text = std::string(1, change),
                .tone = Tone::accent,
                .strong = file.unstaged == '.',  // all of it staged
                .faded = false,
                .tint = true};
    switch (change) {
        case 'A':
            badge.tone = Tone::added;
            break;
        case 'D':
            badge.tone = Tone::removed;
            break;
        case 'R':
        case 'C':
            badge.tone = Tone::renamed;
            break;
        case 'M':
        case 'T':
            break;
        default:
            return std::nullopt;
    }
    return badge;
}

std::string meaning_of(const FileStatus& file) {
    switch (file.kind) {
        case FileStatus::Kind::conflict:
            return std::string{i18n::tr("Conflict")};
        case FileStatus::Kind::untracked:
            return std::string{i18n::tr("Untracked")};
        case FileStatus::Kind::ignored:
            return std::string{i18n::tr("Ignored")};
        case FileStatus::Kind::tracked:
            break;
    }
    if (file.unstaged == '.') {
        return i18n::trf("{}, staged", change_name(file.staged));
    }
    if (file.staged == '.') {
        return i18n::trf("{}, not staged", change_name(file.unstaged));
    }
    return i18n::trf("{}, partly staged", change_name(file.unstaged));
}

std::string summary_of(const Status& status, Operation operation, std::string_view short_hash) {
    const Branch& branch = status.branch;
    std::string text = branch.name.empty() ? i18n::trf("Detached at {}", short_hash) : branch.name;
    if (!branch.upstream.empty()) {
        if (branch.ahead > 0) {
            text += std::format(" ↑{}", branch.ahead);
        }
        if (branch.behind > 0) {
            text += std::format(" ↓{}", branch.behind);
        }
    }
    if (const std::string_view doing = operation_name(operation); !doing.empty()) {
        text.append(kSeparator).append(doing);
    }
    const Status::Counts counts = status.counts();
    const auto add = [&text](std::size_t count, const std::string& words) {
        if (count > 0) {
            text.append(kSeparator).append(words);
        }
    };
    add(counts.conflicts, i18n::trn("{} conflict", "{} conflicts", counts.conflicts));
    add(counts.staged, i18n::trn("{} staged", "{} staged", counts.staged));
    add(counts.changed, i18n::trn("{} changed", "{} changed", counts.changed));
    add(counts.untracked, i18n::trn("{} untracked", "{} untracked", counts.untracked));
    return text;
}

std::string time_ago(std::int64_t then, std::int64_t now) {
    const std::int64_t seconds = std::max<std::int64_t>(0, now - then);
    const auto count = [](std::int64_t value) { return static_cast<std::uint64_t>(value); };
    constexpr std::int64_t kMinute = 60;
    constexpr std::int64_t kHour = 60 * kMinute;
    constexpr std::int64_t kDay = 24 * kHour;
    if (seconds < kMinute) {
        return std::string{i18n::tr("just now")};
    }
    if (seconds < kHour) {
        return i18n::trn("{} minute ago", "{} minutes ago", count(seconds / kMinute));
    }
    if (seconds < kDay) {
        return i18n::trn("{} hour ago", "{} hours ago", count(seconds / kHour));
    }
    if (seconds < 30 * kDay) {
        return i18n::trn("{} day ago", "{} days ago", count(seconds / kDay));
    }
    if (seconds < 365 * kDay) {
        return i18n::trn("{} month ago", "{} months ago", count(seconds / (30 * kDay)));
    }
    return i18n::trn("{} year ago", "{} years ago", count(seconds / (365 * kDay)));
}

std::string commit_line(const Commit& commit, std::int64_t now) {
    return std::format("{} {}{}{}{}{}", commit.hash, commit.subject, kSeparator, commit.author,
                       kSeparator, time_ago(commit.time, now));
}

Decorator::Decorator(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                     std::filesystem::path git, std::function<void()> redraw, Options options)
    : loop_{loop},
      pool_{pool},
      watcher_{watcher},
      git_{std::move(git)},
      redraw_{std::move(redraw)},
      options_{options} {}

Decorator::~Decorator() {
    *alive_ = false;
    for (auto& [key, repo] : repos_) {
        for (const auto id : repo->watches) {
            watcher_.unwatch(id);
        }
        loop_.remove(repo->settle);
        loop_.remove(repo->limit);
    }
}

void Decorator::set_options(const Options& options) {
    if (options == options_) {
        return;
    }
    const bool requery =
        (options.ignored == Options::Ignored::show) != (options_.ignored == Options::Ignored::show);
    const bool refilter =
        (options.ignored == Options::Ignored::hide) != (options_.ignored == Options::Ignored::hide);
    options_ = options;
    if (refilter && refilter_) {
        refilter_();
    }
    for (auto& [key, repo] : repos_) {
        if (requery) {
            schedule(*repo);
        } else {
            rebuild(*repo);
        }
    }
    redraw_();
}

Decorator::Place* Decorator::place_of(const views::Folder& folder) {
    if (folder.is_trash() || folder.is_archive() || in_a_trash(folder.path())) {
        return nullptr;  // not a working tree, whatever path it has
    }
    const std::uint64_t revision = folder.model().revision();
    if (&folder == last_folder_ && revision == last_revision_ && last_place_ != nullptr) {
        return last_place_;
    }
    if (places_.size() > kPlaces) {
        places_.clear();
        last_place_ = nullptr;
    }
    auto& slot = places_[folder.path().native()];
    if (!slot) {
        slot = std::make_unique<Place>();
        slot->folder = folder.path().native();
        slot->revision = revision;
        detect(folder.path());
    } else if (slot->revision != revision) {
        // The listing changed: a file in a repository, or .git itself appearing or going.
        slot->revision = revision;
        if (slot->repo != nullptr) {
            schedule(*slot->repo);
        }
        if (slot->known) {
            detect(folder.path());
        }
    }
    if (slot->repo != nullptr) {
        slot->repo->asked = ++asked_;
    }
    last_folder_ = &folder;
    last_revision_ = revision;
    last_place_ = slot.get();
    return last_place_;
}

void Decorator::detect(const std::filesystem::path& folder) {
    Place& asked = *places_.at(folder.native());
    if (asked.detecting) {
        return;
    }
    asked.detecting = true;
    pool_.submit(
        core::Priority::visible,
        [folder](const core::CancelToken& /*token*/) { return find_repository(folder); },
        [this, alive = alive_, key = folder.native()](std::optional<Repository> found) {
            if (!*alive) {
                return;
            }
            const auto it = places_.find(key);
            if (it == places_.end()) {
                return;
            }
            Place& place = *it->second;
            place.detecting = false;
            place.known = true;
            Repo* const before = place.repo;
            place.repo = found ? &follow(*found) : nullptr;
            place.prefix.clear();
            if (found) {
                const std::filesystem::path inside =
                    std::filesystem::path{key}.lexically_relative(found->worktree);
                if (!inside.empty() && inside != ".") {
                    place.prefix = inside.generic_string() + "/";
                }
            }
            place.ignored_generation = ~std::uint64_t{0};
            if (place.repo != before) {
                last_place_ = nullptr;
                if (hides_any() && place.repo != nullptr && place.repo->status && refilter_) {
                    refilter_();
                }
                redraw_();
            }
        });
}

Decorator::Repo& Decorator::follow(const Repository& repository) {
    const std::string key = repository.worktree.native();
    if (const auto it = repos_.find(key); it != repos_.end()) {
        return *it->second;
    }
    auto repo = std::make_unique<Repo>();
    repo->repository = repository;
    repo->key = key;
    repo->asked = ++asked_;
    Repo& added = *repo;
    repos_.emplace(key, std::move(repo));
    // HEAD, the index and an operation's files; packed-refs and FETCH_HEAD; branches; the
    // reflog, written whenever HEAD moves (a commit on a branch in a folder, "feature/x").
    std::vector<std::filesystem::path> watched{repository.git_dir};
    if (repository.common_dir != repository.git_dir) {
        watched.push_back(repository.common_dir);
    }
    watched.push_back(repository.common_dir / "refs" / "heads");
    watched.push_back(repository.git_dir / "logs");
    for (const auto& dir : watched) {
        std::error_code error;
        const auto id = watcher_.watch(
            dir,
            [this, &added](const std::vector<fs::DirectoryEvent>& events) {
                const bool relevant = std::ranges::any_of(events, [](const auto& event) {
                    return !std::string_view{event.name}.ends_with(".lock");
                });
                if (relevant) {
                    schedule(added);
                }
            },
            error);
        if (id != 0) {
            added.watches.push_back(id);
        } else {
            log::debug("git: cannot watch {}: {}", dir.string(), error.message());
        }
    }
    forget_oldest();
    run(added);
    return added;
}

void Decorator::forget_oldest() {
    while (repos_.size() > kRepositories) {
        Repo* repo = nullptr;
        for (const auto& [key, each] : repos_) {
            if (repo == nullptr || each->asked < repo->asked) {
                repo = each.get();
            }
        }
        if (repo == nullptr) {
            return;
        }
        for (const auto id : repo->watches) {
            watcher_.unwatch(id);
        }
        loop_.remove(repo->settle);
        loop_.remove(repo->limit);
        for (auto& [key, place] : places_) {
            if (place->repo == repo) {
                place->repo = nullptr;
                place->known = false;
                place->revision = 0;  // asked again, it is found again
            }
        }
        last_place_ = nullptr;
        if (part_repo_ == repo) {
            part_repo_ = nullptr;
        }
        if (section_repo_ == repo) {
            section_repo_ = nullptr;
        }
        const std::string key = repo->key;
        repos_.erase(key);
    }
}

void Decorator::schedule(Repo& repo) {
    if (repo.paused) {
        return;
    }
    if (repo.reading) {
        repo.again = true;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (repo.settle == 0) {
        repo.pending_since = now;
    } else {
        loop_.remove(repo.settle);
    }
    const auto most = repo.pending_since + kMostDelay;
    const auto delay = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(most - now),
                                  std::chrono::milliseconds{0}, kSettle);
    repo.settle = loop_.add_timer(delay, std::chrono::milliseconds{0}, [this, &repo] {
        repo.settle = 0;
        run(repo);
    });
}

void Decorator::run(Repo& repo) {
    if (repo.reading) {
        repo.again = true;
        return;
    }
    loop_.remove(repo.settle);
    repo.settle = 0;
    repo.again = false;
    repo.reading = true;
    ++queries_;
    repo.query = std::make_unique<platform::ProgramOutput>(
        loop_, [this, &repo](const std::string& output, int wait_status) {
            loop_.remove(repo.limit);
            repo.limit = 0;
            if (repo.paused) {
                repo.reading = false;
                return;
            }
            if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
                finished(repo, {}, wait_status, Operation::none);
                return;
            }
            // What git is doing is a few stat calls: on a worker, the tree may be remote.
            pool_.submit(
                core::Priority::visible,
                [repository = repo.repository](const core::CancelToken& /*token*/) {
                    return operation_of(repository);
                },
                [this, alive = alive_, key = repo.key, output](Operation operation) {
                    if (!*alive) {
                        return;
                    }
                    if (const auto it = repos_.find(key); it != repos_.end()) {
                        finished(*it->second, output, 0, operation);
                    }
                });
        });
    const bool ignored = options_.ignored != Options::Ignored::show;
    if (!repo.query->start(git_, status_arguments(repo.repository.worktree, ignored))) {
        repo.query.reset();
        repo.reading = false;
        repo.failed = true;
        rebuild(repo);
        return;
    }
    repo.limit = loop_.add_timer(time_limit_, std::chrono::milliseconds{0}, [this, &repo] {
        repo.limit = 0;
        repo.paused = true;
        repo.paused_in = last_place_ != nullptr ? last_place_->folder : std::string{};
        log::info("git status in {} took more than {} ms: paused", repo.key, time_limit_.count());
        repo.query->stop();
        rebuild(repo);
    });
}

void Decorator::finished(Repo& repo, const std::string& output, int wait_status,
                         Operation operation) {
    repo.reading = false;
    repo.operation = operation;
    std::optional<Status> status =
        wait_status == 0 ? parse_status(output) : std::optional<Status>{};
    repo.failed = !status.has_value();
    if (repo.failed) {
        log::debug("git status in {} failed ({})", repo.key, wait_status);
    }
    repo.status = std::move(status);
    rebuild(repo);
    read_commit(repo);
    if (repo.again) {
        schedule(repo);
    }
}

void Decorator::read_commit(Repo& repo) {
    if (!repo.status || repo.status->branch.oid.empty()) {
        repo.commit.reset();
        return;
    }
    if ((repo.commit && repo.status->branch.oid.starts_with(repo.commit->hash)) ||
        repo.commit_query) {
        return;
    }
    repo.commit_query = std::make_unique<platform::ProgramOutput>(
        loop_, [this, &repo](const std::string& output, int wait_status) {
            repo.commit = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0
                              ? parse_commit(output)
                              : std::nullopt;
            ++repo.generation;
            redraw_();
            repo.commit_query.reset();  // ends this callback's object: nothing after
        });
    if (!repo.commit_query->start(git_, commit_arguments(repo.repository.worktree))) {
        repo.commit_query.reset();
    }
}

void Decorator::rebuild(Repo& repo) {
    ++repo.generation;
    repo.files.clear();
    repo.folders.clear();
    repo.index.clear();
    repo.ignored_folders.clear();
    std::size_t signature = 0;
    if (repo.status && !repo.failed) {
        const std::vector<FileStatus>& files = repo.status->files;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const FileStatus& file = files[i];
            const std::optional<Badge> badge = badge_of(file);
            if (!badge) {
                continue;
            }
            repo.index.emplace(file.path, i);
            if (file.kind == FileStatus::Kind::ignored) {
                signature = (signature * 31U) ^ std::hash<std::string>{}(file.path);
                if (file.path.ends_with('/')) {
                    repo.ignored_folders.push_back(file.path);
                }
                repo.files.emplace(file.path, *badge);
                continue;
            }
            repo.files.emplace(file.path, *badge);
            if (!options_.folder_status) {
                continue;
            }
            // Every folder above it shows that something inside changed.
            for (std::size_t slash = file.path.find('/'); slash != std::string::npos;
                 slash = file.path.find('/', slash + 1)) {
                Badge summary{.text = "•",
                              .tone = badge->tone,
                              .strong = false,
                              .faded = false,
                              .tint = true};
                auto [it, added] = repo.folders.try_emplace(file.path.substr(0, slash), summary);
                if (!added && weight(summary) > weight(it->second)) {
                    it->second = summary;
                }
            }
        }
    }
    std::size_t ranks = repo.files.size() * 31U + repo.folders.size();
    for (const auto& map : {&repo.files, &repo.folders}) {
        for (const auto& [path, badge] : *map) {
            ranks += std::hash<std::string>{}(path) ^ (std::hash<std::string>{}(badge.text) << 1U);
        }
    }
    const bool hidden_changed = signature != repo.ignored_signature;
    const bool ranks_changed = ranks != repo.rank_signature;
    repo.ignored_signature = signature;
    repo.rank_signature = ranks;
    // Listings filter and sort with what changed: hidden entries, and the order by status.
    if (((hidden_changed && hides_any()) || ranks_changed) && refilter_) {
        refilter_();
    }
    redraw_();
}

const views::Badge* Decorator::lookup(const Repo& repo, const Place& place,
                                      const std::string& name) const {
    if (const auto it = repo.files.find(name); it != repo.files.end()) {
        return &it->second;
    }
    if (const auto it = repo.files.find(name + "/"); it != repo.files.end()) {
        return &it->second;  // an ignored folder
    }
    if (const auto it = repo.folders.find(name); it != repo.folders.end()) {
        return &it->second;
    }
    if (place.ignored) {
        static const Badge kIgnored{
            .text = {}, .tone = Tone::muted, .strong = false, .faded = true};
        return &kIgnored;
    }
    return nullptr;
}

const views::Badge* Decorator::badge_at(Place& place, const std::string& name) {
    if (place.repo == nullptr) {
        return nullptr;
    }
    const Repo& repo = *place.repo;
    if (repo.failed || repo.paused || (repo.files.empty() && repo.ignored_folders.empty())) {
        return nullptr;
    }
    if (place.ignored_generation != repo.generation) {
        place.ignored_generation = repo.generation;
        place.ignored = std::ranges::any_of(repo.ignored_folders, [&](const std::string& dir) {
            return place.prefix.starts_with(dir);
        });
    }
    const Badge* const found = lookup(repo, place, place.prefix + name);
    if (found != nullptr && found->faded && options_.ignored == Options::Ignored::show) {
        return nullptr;
    }
    return found;
}

const views::Badge* Decorator::badge(const views::Folder& folder, const fs::Entry& entry) {
    Place* const place = place_of(folder);
    return place != nullptr ? badge_at(*place, entry.name) : nullptr;
}

bool Decorator::hides(const std::filesystem::path& folder, const fs::Entry& entry) {
    if (!hides_any()) {
        return false;
    }
    const auto it = places_.find(folder.native());
    if (it == places_.end()) {
        return false;
    }
    const Badge* const found = badge_at(*it->second, entry.name);
    return found != nullptr && found->faded;
}

int Decorator::rank(const std::filesystem::path& folder, const fs::Entry& entry) {
    const auto it = places_.find(folder.native());
    const Badge* const found = it != places_.end() ? badge_at(*it->second, entry.name) : nullptr;
    if (found == nullptr) {
        return 3;
    }
    if (found->faded) {
        return 4;
    }
    if (found->text == "!") {
        return 0;
    }
    return found->tone == views::Badge::Tone::added ? 2 : 1;
}

const views::StatusPart* Decorator::status(const views::Folder& folder) {
    Place* const place = place_of(folder);
    if (place == nullptr || place->repo == nullptr) {
        return nullptr;
    }
    Repo& repo = *place->repo;
    if (repo.paused && repo.paused_in != place->folder) {
        repo.paused = false;  // another folder: try again
        schedule(repo);
    }
    if (repo.paused) {
        part_repo_ = nullptr;
        part_ = {.lead = {},
                 .text = std::string{i18n::tr("Git: paused")},
                 .detail = i18n::trf(
                     "git status took more than {} seconds here; open another folder to retry",
                     kTimeLimit.count() / 1000)};
        return &part_;
    }
    if (repo.failed || !repo.status) {
        return nullptr;
    }
    const fs::Entry* const cursor = folder.cursor_entry();
    const std::string_view cursor_name = cursor != nullptr ? cursor->name : std::string_view{};
    const std::int64_t now = seconds_now();
    if (part_repo_ == &repo && part_generation_ == repo.generation && part_cursor_ == cursor_name &&
        part_minute_ == now / 60) {
        return &part_;
    }
    part_repo_ = &repo;
    part_generation_ = repo.generation;
    part_cursor_ = cursor_name;
    part_minute_ = now / 60;
    std::string text =
        summary_of(*repo.status, repo.operation,
                   repo.commit ? std::string_view{repo.commit->hash}
                               : std::string_view{repo.status->branch.oid}.substr(
                                     0, std::min<std::size_t>(7, repo.status->branch.oid.size())));
    if (cursor != nullptr) {
        const std::string path = place->prefix + cursor->name;
        if (const auto it = repo.index.find(path); it != repo.index.end()) {
            text.append(kSeparator).append(meaning_of(repo.status->files[it->second]));
        } else if (cursor->is_directory() && options_.folder_status &&
                   repo.folders.contains(path)) {
            text.append(kSeparator).append(i18n::tr("Changes inside"));
        }
    }
    // The branch (and ahead/behind) stands out; a clean tree says so.
    const std::size_t head = std::min(text.find(kSeparator), text.size());
    part_.lead = text.substr(0, head);
    if (repo.status->counts() == Status::Counts{} && repo.operation == Operation::none) {
        part_.lead += " ✓";
    }
    part_.text = text.substr(head);
    part_.detail = repo.commit ? commit_line(*repo.commit, now) : std::string{};
    return &part_;
}

const views::DetailSection* Decorator::details(const views::Folder& folder) {
    Place* const place = place_of(folder);
    if (place == nullptr || place->repo == nullptr) {
        return nullptr;
    }
    Repo& repo = *place->repo;
    if (repo.failed || !repo.status) {
        return nullptr;
    }
    const std::int64_t now = seconds_now();
    if (section_repo_ == &repo && section_generation_ == repo.generation &&
        section_minute_ == now / 60) {
        return &section_;
    }
    section_repo_ = &repo;
    section_generation_ = repo.generation;
    section_minute_ = now / 60;
    const Branch& branch = repo.status->branch;
    std::vector<views::DetailSection::Row> rows;
    const auto add = [&rows](std::string_view label, std::string value) {
        if (!value.empty()) {
            rows.push_back({std::string{label}, std::move(value)});
        }
    };
    add(i18n::tr("Branch"),
        branch.name.empty()
            ? i18n::trf("Detached at {}", repo.commit ? repo.commit->hash : branch.oid.substr(0, 7))
            : branch.name);
    if (!branch.upstream.empty()) {
        add(i18n::tr("Upstream"),
            std::format("{}  ↑{} ↓{}", branch.upstream, branch.ahead, branch.behind));
    }
    if (const std::string_view doing = operation_name(repo.operation); !doing.empty()) {
        add(i18n::tr("In progress"), std::string{doing});
    }
    const Status::Counts counts = repo.status->counts();
    std::string changes;
    const auto count = [&changes](std::size_t n, const std::string& words) {
        if (n > 0) {
            changes.append(changes.empty() ? "" : ", ").append(words);
        }
    };
    count(counts.conflicts, i18n::trn("{} conflict", "{} conflicts", counts.conflicts));
    count(counts.staged, i18n::trn("{} staged", "{} staged", counts.staged));
    count(counts.changed, i18n::trn("{} changed", "{} changed", counts.changed));
    count(counts.untracked, i18n::trn("{} untracked", "{} untracked", counts.untracked));
    add(i18n::tr("Changes"), changes.empty() ? std::string{i18n::tr("None")} : changes);
    if (repo.commit) {
        add(i18n::tr("Last commit"), repo.commit->hash + " " + repo.commit->subject);
        add(i18n::tr("Author"), repo.commit->author);
        add(i18n::tr("Committed"), time_ago(repo.commit->time, now));
    }
    if (!repo.action_name.empty()) {
        add(i18n::tr("Running"), repo.action_name + "…");
    }
    section_.title = std::string{i18n::tr("GIT")};
    section_.rows = std::move(rows);
    section_.buttons = buttons(*place->repo);
    return &section_;
}

}  // namespace rummage::git
