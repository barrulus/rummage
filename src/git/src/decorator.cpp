#include "rummage/git/decorator.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>
#include <sys/wait.h>
#include <utility>

#include "repo.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/log.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/launcher.hpp"
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
    switch (badge.tone) {
        case Tone::conflict:
            return 4;
        case Tone::deleted:
            return 3;
        case Tone::added:
        case Tone::untracked:
            return 1;
        default:
            return 2;
    }
}

std::string_view change_name(char change) {
    switch (change) {
        case 'M':
            return i18n::trc("git", "Modified");
        case 'T':
            return i18n::trc("git", "Type changed");
        case 'A':
            return i18n::trc("git", "Added");
        case 'D':
            return i18n::trc("git", "Deleted");
        case 'R':
            return i18n::trc("git", "Renamed");
        case 'C':
            return i18n::trc("git", "Copied");
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

Badge in_scheme(Badge badge, const FileStatus& file, Options::Colors colors) {
    if (colors != Options::Colors::git || badge.faded) {
        return badge;
    }
    // git's own: what is staged green, the rest (not staged, untracked, unmerged) red.
    const bool staged = file.kind == FileStatus::Kind::tracked && file.unstaged == '.';
    badge.tone = staged ? Tone::added : Tone::deleted;
    return badge;
}

std::optional<Badge> badge_of(const FileStatus& file) {
    switch (file.kind) {
        case FileStatus::Kind::conflict:
            return Badge{
                .text = "!", .tone = Tone::conflict, .strong = true, .faded = false, .tint = true};
        case FileStatus::Kind::untracked:
            return Badge{.text = "U",
                         .tone = Tone::untracked,
                         .strong = false,
                         .faded = false,
                         .tint = true};
        case FileStatus::Kind::ignored:
            return Badge{.text = {}, .tone = Tone::muted, .strong = false, .faded = true};
        case FileStatus::Kind::tracked:
            break;
    }
    const char change = file.unstaged != '.' ? file.unstaged : file.staged;
    Badge badge{.text = std::string(1, change),
                .tone = Tone::modified,
                .strong = file.unstaged == '.',  // all of it staged
                .faded = false,
                .tint = true};
    switch (change) {
        case 'A':
            badge.tone = Tone::added;
            break;
        case 'D':
            badge.tone = Tone::deleted;
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
            return std::string{i18n::trc("git", "Conflict")};
        case FileStatus::Kind::untracked:
            return std::string{i18n::trc("git", "Untracked")};
        case FileStatus::Kind::ignored:
            return std::string{i18n::trc("git", "Ignored")};
        case FileStatus::Kind::tracked:
            break;
    }
    if (file.unstaged == '.') {
        return i18n::trcf("git", "{}, staged", change_name(file.staged));
    }
    if (file.staged == '.') {
        return i18n::trcf("git", "{}, not staged", change_name(file.unstaged));
    }
    return i18n::trcf("git", "{}, partly staged", change_name(file.unstaged));
}

std::string summary_of(const Status& status, Operation operation, std::string_view short_hash) {
    const Branch& branch = status.branch;
    std::string text =
        branch.name.empty() ? i18n::trcf("git", "Detached at {}", short_hash) : branch.name;
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
    add(counts.conflicts, i18n::trnc("git", "{} conflict", "{} conflicts", counts.conflicts));
    add(counts.staged, i18n::trnc("git", "{} staged", "{} staged", counts.staged));
    add(counts.changed, i18n::trnc("git", "{} changed", "{} changed", counts.changed));
    add(counts.untracked, i18n::trnc("git", "{} untracked", "{} untracked", counts.untracked));
    return text;
}

std::string time_ago(std::int64_t then, std::int64_t now) {
    const std::int64_t seconds = std::max<std::int64_t>(0, now - then);
    const auto count = [](std::int64_t value) { return static_cast<std::uint64_t>(value); };
    constexpr std::int64_t kMinute = 60;
    constexpr std::int64_t kHour = 60 * kMinute;
    constexpr std::int64_t kDay = 24 * kHour;
    if (seconds < kMinute) {
        return std::string{i18n::trc("git", "just now")};
    }
    if (seconds < kHour) {
        return i18n::trnc("git", "{} minute ago", "{} minutes ago", count(seconds / kMinute));
    }
    if (seconds < kDay) {
        return i18n::trnc("git", "{} hour ago", "{} hours ago", count(seconds / kHour));
    }
    if (seconds < 30 * kDay) {
        return i18n::trnc("git", "{} day ago", "{} days ago", count(seconds / kDay));
    }
    if (seconds < 365 * kDay) {
        return i18n::trnc("git", "{} month ago", "{} months ago", count(seconds / (30 * kDay)));
    }
    return i18n::trnc("git", "{} year ago", "{} years ago", count(seconds / (365 * kDay)));
}

namespace {

// name with what a POSIX extended regular expression would read as special escaped.
std::string regex_escaped(std::string_view name) {
    std::string escaped;
    for (const char c : name) {
        if (std::string_view{".[]{}()\\*+?^$|"}.find(c) != std::string_view::npos) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

}  // namespace

RemoteView view_of_remote(std::string_view remote) {
    // The hosts whose web pages sit where their ssh addresses point.
    constexpr std::array<std::string_view, 5> kKnown{"github.com", "gitlab.com", "bitbucket.org",
                                                     "codeberg.org", "git.sr.ht"};
    RemoteView view;
    std::string_view scheme;
    std::string_view host;
    std::string_view path;
    if (const std::size_t marker = remote.find("://"); marker != std::string_view::npos) {
        scheme = remote.substr(0, marker);
        const std::string_view rest = remote.substr(marker + 3);
        const std::size_t slash = rest.find('/');
        std::string_view authority = rest.substr(0, slash);
        if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
            authority.remove_prefix(at + 1);  // a user, password or token is never shown
        }
        host = authority;
        path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        view.shown = std::string{scheme} + "://" + std::string{host} + "/" + std::string{path};
    } else if (const std::size_t colon = remote.find(':');
               colon != std::string_view::npos && remote.find('/') > colon) {
        // scp-like: [user@]host:owner/repo
        std::string_view authority = remote.substr(0, colon);
        if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
            authority.remove_prefix(at + 1);
        }
        scheme = "ssh";
        host = authority;
        path = remote.substr(colon + 1);
        view.shown = std::string{remote};
    } else {
        view.shown = std::string{remote};  // a path
        return view;
    }
    if (scheme == "file" || host.empty()) {
        return view;
    }
    std::string page{path};
    while (page.ends_with('/')) {
        page.pop_back();
    }
    if (page.ends_with(".git")) {
        page.resize(page.size() - 4);
    }
    while (page.starts_with('/')) {
        page.erase(0, 1);
    }
    std::string bare_host{host};
    if (const std::size_t colon = bare_host.find(':'); colon != std::string::npos) {
        bare_host.resize(colon);  // an ssh port is not the web page's
    }
    const bool known = std::ranges::find(kKnown, bare_host) != kKnown.end();
    if (page.empty() || !(scheme == "https" || known)) {
        return view;
    }
    view.link = "https://" + (scheme == "https" ? std::string{host} : bare_host) + "/" + page;
    view.shown = view.link->substr(std::string_view{"https://"}.size());
    return view;
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
    part_repo_ = nullptr;  // texts may be in another language now
    section_repo_ = nullptr;
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
                if (std::ranges::any_of(events,
                                        [](const auto& event) { return event.name == "config"; })) {
                    added.remote_stale = true;
                }
                added.refs_stale = true;  // these folders hold HEAD and the refs
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
    read_remote(repo);
    read_branches(repo);
    if (repo.again) {
        schedule(repo);
    }
}

void Decorator::read_remote(Repo& repo) {
    if (!repo.status || repo.remote_query) {
        return;
    }
    const std::string& upstream = repo.status->branch.upstream;
    const std::string name =
        upstream.empty() ? std::string{"origin"} : upstream.substr(0, upstream.find('/'));
    if (name == repo.remote_name && !repo.remote_stale) {
        return;
    }
    repo.remote_name = name;
    repo.remote_stale = false;
    repo.remote_query = std::make_unique<platform::ProgramOutput>(
        loop_, [this, &repo](const std::string& output, int wait_status) {
            // "key value" lines: the remote's URL, and whether pull's way is configured.
            std::string url;
            bool pull_configured = false;
            if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
                std::istringstream lines{output};
                for (std::string line; std::getline(lines, line);) {
                    const std::size_t space = line.find(' ');
                    const std::string key_name = line.substr(0, space);
                    if (key_name.starts_with("pull.")) {
                        pull_configured = true;
                    } else if (space != std::string::npos) {
                        url = line.substr(space + 1);
                    }
                }
            }
            repo.pull_configured = pull_configured;
            repo.remote =
                url.empty() ? std::nullopt : std::optional<RemoteView>{view_of_remote(url)};
            ++repo.generation;
            redraw_();
            repo.remote_query.reset();  // ends this callback's object: nothing after
        });
    if (!repo.remote_query->start(
            git_, {"--no-optional-locks", "-C", repo.repository.worktree.string(), "config",
                   "--get-regexp",
                   "^(remote\\." + regex_escaped(name) + "\\.url|pull\\.(rebase|ff))$"})) {
        repo.remote_query.reset();
    }
}

Branches parse_branches(std::string_view output) {
    Branches branches;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        const std::string_view ref = output.substr(start, end - start);
        start = end + 1;
        if (ref.starts_with("refs/heads/")) {
            branches.local.emplace_back(ref.substr(11));
        } else if (ref.starts_with("refs/remotes/") && !ref.ends_with("/HEAD")) {
            branches.remote.emplace_back(ref.substr(13));
        }
    }
    std::ranges::sort(branches.local);
    std::ranges::sort(branches.remote);
    return branches;
}

void Decorator::read_branches(Repo& repo) {
    if (!repo.refs_stale || repo.branches_query) {
        return;
    }
    repo.refs_stale = false;
    repo.branches_query = std::make_unique<platform::ProgramOutput>(
        loop_, [this, &repo](const std::string& output, int wait_status) {
            if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
                Branches branches = parse_branches(output);
                repo.local_branches = std::move(branches.local);
                repo.remote_branches = std::move(branches.remote);
            }
            repo.branches_query.reset();  // ends this callback's object: nothing after
        });
    if (!repo.branches_query->start(
            git_, {"--no-optional-locks", "-C", repo.repository.worktree.string(), "for-each-ref",
                   "--format=%(refname)", "refs/heads", "refs/remotes"})) {
        repo.branches_query.reset();
    }
}

std::vector<views::DetailSection::Button> Decorator::entry_actions(
    Repo& repo, int which, const std::string& path, const std::filesystem::path& target) {
    const std::string key = repo.key;
    const bool idle = !repo.action && repo.action_name.empty();
    const auto on = [this, key](auto then) {
        return [this, key, then] {
            if (const auto it = repos_.find(key); it != repos_.end() && it->second->status) {
                then(*it->second);
            }
        };
    };
    // Glyphs as in editors: + stage, − unstage, ↶ discard, ⇄ changes, ↗ open.
    std::vector<views::DetailSection::Button> actions;
    const auto add = [&](std::string id, std::string glyph, std::string_view hint, bool enabled,
                         std::function<void()> action) {
        actions.push_back({.id = "git_" + id,
                           .label = std::move(glyph),
                           .enabled = enabled,
                           .action = std::move(action),
                           .hint = std::string{hint},
                           .menu = {}});
    };
    const bool has_head = repo.status && !repo.status->branch.oid.empty();
    if (which == 1) {
        add("unstage", "−", i18n::trc("git", "Unstage"), idle, on([this, path, has_head](Repo& at) {
                act(at, std::string{i18n::trc("git", "Unstage")},
                    {has_head ? std::vector<std::string>{"--literal-pathspecs", "restore",
                                                         "--staged", "--", path}
                              : std::vector<std::string>{"--literal-pathspecs", "rm", "--cached",
                                                         "-q", "--", path}},
                    {});
            }));
    } else {
        add("stage", "+", i18n::trc("git", "Stage"), idle, on([this, path](Repo& at) {
                act(at, std::string{i18n::trc("git", "Stage")},
                    {{"--literal-pathspecs", "add", "-A", "--", path}}, {});
            }));
    }
    add("discard", "↶", i18n::trc("git", "Discard Changes…"), idle,
        on([this, path](Repo& at) { discard(at, {path}); }));
    add("diff", "⇄", i18n::trc("git", "Show Changes"), true,
        on([this, which, path](Repo& at) { show_group_diff(at, which, path); }));
    add("open", "↗", i18n::trc("git", "Open File"), std::filesystem::exists(target),
        [this, target] {
            if (host_.open_file) {
                host_.open_file(target);
            }
        });
    return actions;
}

void Decorator::show_group_diff(Repo& repo, int which, const std::string& path) {
    if (!repo.status) {
        return;
    }
    // The group's files, as its list shows them, each with the changes it lists.
    std::vector<DiffItem> items;
    std::size_t index = 0;
    for (const FileStatus& file : repo.status->files) {
        const bool conflict = file.kind == FileStatus::Kind::conflict;
        const bool staged = file.kind == FileStatus::Kind::tracked && file.staged != '.';
        const bool changed = file.kind == FileStatus::Kind::untracked ||
                             (file.kind == FileStatus::Kind::tracked && file.unstaged != '.');
        if ((which == 0 && !conflict) || (which == 1 && !staged) || (which == 2 && !changed)) {
            continue;
        }
        const DiffKind kind = which == 1                                 ? DiffKind::staged
                              : file.kind == FileStatus::Kind::untracked ? DiffKind::untracked
                                                                         : DiffKind::changes;
        if (file.path == path) {
            index = items.size();
        }
        items.push_back({file.path, kind});
    }
    show_diff(repo.key, std::move(items), index);
}

void Decorator::open_link(const std::string& url) {
    // xdg-open detached from rummage, its streams on /dev/null: a browser it starts may write
    // long after, and comes to the front with the window's activation token.
    const auto launch = [this, alive = alive_, url](const std::string& token) {
        if (!*alive) {
            return;
        }
        if (!launcher_) {
            launcher_ = std::make_unique<platform::Launcher>(loop_);
        }
        std::vector<std::string> env;
        if (!token.empty()) {
            env.push_back("XDG_ACTIVATION_TOKEN=" + token);
            env.push_back("DESKTOP_STARTUP_ID=" + token);
        }
        const std::error_code error =
            launcher_->spawn({"xdg-open", url}, {}, env, [](const platform::Launcher::Exit&) {});
        if (error && host_.message) {
            host_.message(i18n::trcf("git", "Cannot open {}: xdg-open is missing", url), true);
        }
    };
    if (activation_) {
        activation_(launch);
    } else {
        launch({});
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
            std::optional<Badge> badge = badge_of(file);
            if (badge) {
                badge = in_scheme(*badge, file, options_.colors);
            }
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
    // By the letter, whatever colours show it: new files after changes.
    return found->text == "A" || found->text == "U" ? 2 : 1;
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
        part_ = {
            .lead = {},
            .text = std::string{i18n::trc("git", "Git: paused")},
            .detail = i18n::trcf(
                "git", "git status took more than {} seconds here; open another folder to retry",
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
            text.append(kSeparator).append(i18n::trc("git", "Changes inside"));
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
            rows.push_back({std::string{label}, std::move(value), nullptr});
        }
    };
    if (!branch.upstream.empty()) {
        add(i18n::trc("git", "Upstream"),
            std::format("{}  ↑{} ↓{}", branch.upstream, branch.ahead, branch.behind));
    }
    if (repo.remote) {
        std::function<void()> open;
        if (repo.remote->link) {
            open = [this, url = *repo.remote->link] { open_link(url); };
        }
        rows.push_back(
            {std::string{i18n::trc("git", "Repository")}, repo.remote->shown, std::move(open)});
    }
    if (const std::string_view doing = operation_name(repo.operation); !doing.empty()) {
        add(i18n::trc("git", "In progress"), std::string{doing});
    }
    const Status::Counts counts = repo.status->counts();
    if (repo.commit) {
        add(i18n::trc("git", "Last commit"), repo.commit->hash + " " + repo.commit->subject);
        add(i18n::trc("git", "Author"), repo.commit->author);
        add(i18n::trc("git", "Committed"), time_ago(repo.commit->time, now));
    }
    if (!repo.action_name.empty()) {
        add(i18n::trc("git", "Running"), repo.action_name + "…");
    }
    section_.title = std::string{i18n::trc("git", "GIT")};
    section_.rows = std::move(rows);
    section_.buttons.clear();
    const std::string key = repo.key;
    const auto on = [this, key](auto then) {
        return [this, key, then](auto&&... arguments) {
            if (const auto it = repos_.find(key); it != repos_.end()) {
                then(*it->second, arguments...);
            }
        };
    };
    const bool idle = !repo.action && repo.action_name.empty();
    const bool dirty = counts.staged + counts.changed + counts.untracked + counts.conflicts > 0;
    std::string subtitle = branch.name.empty() ? branch.oid.substr(0, 7) : branch.name;
    subtitle += dirty ? "*" : "";
    if (!branch.upstream.empty() && (branch.ahead > 0 || branch.behind > 0)) {
        subtitle += std::format(" ↑{} ↓{}", branch.ahead, branch.behind);
    }
    section_.header = views::DetailSection::Header{
        .title = repo.repository.worktree.filename().string(),
        .subtitle = std::move(subtitle),
        .buttons = {{.id = "sync",
                     .label = "⟳",
                     .enabled = idle && !branch.upstream.empty(),
                     .action = on([this](Repo& at) { sync(at); }),
                     .hint = std::string{i18n::trc("git", "Sync")},
                     .menu = {}},
                    {.id = "more",
                     .label = "…",
                     .enabled = true,
                     .action = {},
                     .hint = std::string{i18n::trc("git", "More Actions")},
                     .menu =
                         [this, key] {
                             const auto it = repos_.find(key);
                             return it != repos_.end() ? repo_items(*it->second, false)
                                                       : std::vector<views::MenuItem>{};
                         }}},
        .subtitle_menu = [this, key] {
            const auto it = repos_.find(key);
            return it != repos_.end() ? branch_items(*it->second) : std::vector<views::MenuItem>{};
        }};

    section_.input = views::DetailSection::Input{
        .placeholder = std::string{i18n::trc("git", "Message (Ctrl+Enter to commit)")},
        .revision = repo.input_revision,
        .text = repo.input_text,
        .submit = {}};
    // The button does what the settings choose; its menu has every way.
    const CommitMode chosen =
        options_.commit_action == Options::CommitAction::commit_push   ? CommitMode::push
        : options_.commit_action == Options::CommitAction::commit_sync ? CommitMode::sync
                                                                       : CommitMode::plain;
    const std::string_view chosen_label =
        chosen == CommitMode::push   ? i18n::trc("git", "Commit & Push")
        : chosen == CommitMode::sync ? i18n::trc("git", "Commit & Sync")
                                     : i18n::trc("git", "Commit");
    section_.split = views::DetailSection::Split{
        .label = "✓ " + std::string{chosen_label},
        .enabled = idle,
        .action = on([this, chosen](Repo& at, const std::string& typed) {
            commit_typed(at, typed, chosen);
        }),
        .menu = [this, key](const std::string& typed) {
            std::vector<views::MenuItem> items;
            const auto add_mode = [&](std::string id, std::string_view label, CommitMode mode) {
                views::MenuItem item;
                item.id = "git_" + id;
                item.label = std::string{label};
                item.action = [this, key, typed, mode] {
                    if (const auto it = repos_.find(key); it != repos_.end()) {
                        commit_typed(*it->second, typed, mode);
                    }
                };
                items.push_back(std::move(item));
            };
            add_mode("commit", i18n::trc("git", "Commit"), CommitMode::plain);
            add_mode("amend", i18n::trc("git", "Commit (Amend)"), CommitMode::amend);
            add_mode("commit_push", i18n::trc("git", "Commit & Push"), CommitMode::push);
            add_mode("commit_sync", i18n::trc("git", "Commit & Sync"), CommitMode::sync);
            items[2].separator = true;
            return items;
        }};
    section_.input->submit =
        on([this, chosen](Repo& at, const std::string& typed) { commit_typed(at, typed, chosen); });
    // The changed files, in groups; their entries are made only when a group is open.
    section_.groups.clear();
    const auto group = [&](std::string id, std::string_view title, std::size_t count_of,
                           int which) {
        if (count_of == 0) {
            return;
        }
        section_.groups.push_back(
            {.id = "git_" + id,
             .title = std::string{title},
             .count = count_of,
             .entries = [this, key, which] {
                 std::vector<views::DetailSection::Entry> entries;
                 const auto it = repos_.find(key);
                 if (it == repos_.end() || !it->second->status) {
                     return entries;
                 }
                 Repo& at = *it->second;
                 for (const FileStatus& file : at.status->files) {
                     const bool conflict = file.kind == FileStatus::Kind::conflict;
                     const bool staged =
                         file.kind == FileStatus::Kind::tracked && file.staged != '.';
                     const bool changed =
                         file.kind == FileStatus::Kind::untracked ||
                         (file.kind == FileStatus::Kind::tracked && file.unstaged != '.');
                     if ((which == 0 && !conflict) || (which == 1 && !staged) ||
                         (which == 2 && !changed)) {
                         continue;
                     }
                     FileStatus shown = file;
                     if (which == 1) {
                         shown.unstaged = '.';  // the staged change's letter
                     } else if (which == 2 && file.kind == FileStatus::Kind::tracked) {
                         shown.staged = '.';
                     }
                     const std::filesystem::path path{file.path};
                     const std::filesystem::path target = at.repository.worktree / path;
                     entries.push_back(
                         {.name = path.filename().string(),
                          .detail = path.parent_path().string(),
                          .badge = in_scheme(badge_of(shown).value_or(views::Badge{}), shown,
                                             options_.colors),
                          // A click: its changes, the arrows going through this list.
                          .open =
                              [this, key, which, file_path = file.path] {
                                  if (const auto found = repos_.find(key); found != repos_.end()) {
                                      show_group_diff(*found->second, which, file_path);
                                  }
                              },
                          .menu =
                              [this, key, file_path = file.path, root = at.repository.worktree,
                               target] {
                                  std::vector<views::MenuItem> items;
                                  const auto found = repos_.find(key);
                                  if (found == repos_.end() || !found->second->status) {
                                      return items;
                                  }
                                  views::MenuItem reveal;
                                  reveal.id = "git_reveal";
                                  reveal.label = std::string{i18n::trc("git", "Show in Browser")};
                                  reveal.action = [this, root, target] {
                                      if (host_.reveal) {
                                          host_.reveal(root, target);
                                      }
                                  };
                                  items.push_back(std::move(reveal));
                                  views::MenuItem open;
                                  open.id = "git_open";
                                  open.label = std::string{i18n::trc("git", "Open File")};
                                  open.enabled = std::filesystem::exists(target);
                                  open.action = [this, target] {
                                      if (host_.open_file) {
                                          host_.open_file(target);
                                      }
                                  };
                                  items.push_back(std::move(open));
                                  auto git = path_items(*found->second, {file_path}, false);
                                  if (!git.empty()) {
                                      git.front().separator = true;
                                  }
                                  items.insert(items.end(), std::make_move_iterator(git.begin()),
                                               std::make_move_iterator(git.end()));
                                  return items;
                              },
                          .actions = entry_actions(at, which, file.path, target)});
                 }
                 return entries;
             }});
    };
    group("conflicts", i18n::trc("git", "Merge Changes"), counts.conflicts, 0);
    group("staged", i18n::trc("git", "Staged Changes"), counts.staged, 1);
    group("changes", i18n::trc("git", "Changes"), counts.changed + counts.untracked, 2);
    return &section_;
}

}  // namespace rummage::git
