// The commands that change a repository: the git submenu of the context menu and the buttons of
// the information pane's section. Each runs git in the background, its steps in order; the status
// is read again when they end, and what went wrong is shown with git's own words.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <utility>

#include "repo.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/log.hpp"

namespace rummage::git {
namespace {

// Nothing git runs may wait for an answer: no terminal prompt, no SSH question, no editor.
std::vector<std::string> quiet_environment(const std::string& language) {
    std::vector<std::string> variables{"GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=true",
                                       "GIT_MERGE_AUTOEDIT=no"};
    if (std::getenv("GIT_SSH_COMMAND") == nullptr && std::getenv("GIT_SSH") == nullptr) {
        variables.emplace_back("GIT_SSH_COMMAND=ssh -o BatchMode=yes");
    }
    // git's own messages in the language chosen for git: English needs the C locale's messages.
    if (language == "en") {
        variables.emplace_back("LC_MESSAGES=C.UTF-8");
        variables.emplace_back("LANGUAGE=");
    } else if (!language.empty()) {
        variables.emplace_back("LANGUAGE=" + language);
    }
    return variables;
}

// The paths git reports at path or inside it (a folder).
std::vector<const FileStatus*> statuses_at(const Status& status, const std::string& path) {
    std::vector<const FileStatus*> found;
    const std::string inside = path + "/";
    for (const FileStatus& file : status.files) {
        if (file.path == path || file.path == inside || file.path.starts_with(inside)) {
            found.push_back(&file);
        }
    }
    return found;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

}  // namespace

std::string message_of(const std::string& output) {
    std::istringstream in{output};
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        const std::size_t colon = line.find(": ");
        const bool prefixed =
            colon != std::string::npos && colon > 0 && colon <= 16 && line.find(' ') > colon;
        const std::string_view prefix =
            prefixed ? std::string_view{line}.substr(0, colon) : std::string_view{};
        if (line.empty() || prefix == "hint" || prefix == "Hinweis" || line == "hint:" ||
            line == "Hinweis:") {
            continue;
        }
        return prefixed ? line.substr(colon + 2) : line;
    }
    return {};
}

struct Decorator::Acting {
    std::string name;
    Steps steps;
    std::size_t next{0};
    std::string done;
    std::function<void()> after;
    std::chrono::milliseconds limit{Decorator::kTimeLimit};
};

bool Decorator::acting() const {
    return std::ranges::any_of(repos_,
                               [](const auto& each) { return each.second->action != nullptr; });
}

void Decorator::act(Repo& repo, std::string name, Steps steps, std::string done,
                    std::function<void()> after) {
    if (repo.action) {
        if (host_.message) {
            host_.message(i18n::trcf("git", "Git is busy: {}", repo.action_name), true);
        }
        return;
    }
    auto acting = std::make_shared<Acting>();
    const bool network = std::ranges::any_of(steps, [](const auto& arguments) {
        return !arguments.empty() && (arguments.front() == "fetch" || arguments.front() == "pull" ||
                                      arguments.front() == "push");
    });
    acting->limit = network ? kNetworkLimit : kTimeLimit;
    acting->name = std::move(name);
    acting->steps = std::move(steps);
    acting->done = std::move(done);
    acting->after = std::move(after);
    repo.action_name = acting->name;
    ++repo.generation;
    redraw_();
    step(repo, std::move(acting));
}

void Decorator::step(Repo& repo, std::shared_ptr<Acting> acting) {
    const auto finish = [this, &repo](bool ok, const std::string& message, Acting& state) {
        loop_.remove(repo.action_limit);
        repo.action_limit = 0;
        repo.action_name.clear();
        ++repo.generation;
        if (host_.message && !message.empty()) {
            host_.message(message, !ok);
        }
        if (ok && state.after) {
            state.after();
        }
        run(repo);  // what changed, at once
        redraw_();
    };
    if (acting->next >= acting->steps.size()) {
        repo.action.reset();
        finish(true, acting->done, *acting);
        return;
    }
    std::vector<std::string> arguments{"-C", repo.repository.worktree.string()};
    const auto& own = acting->steps[acting->next++];
    arguments.insert(arguments.end(), own.begin(), own.end());
    repo.action = std::make_unique<platform::ProgramOutput>(
        loop_, [this, &repo, acting, finish](const std::string& output, int wait_status) {
            if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
                const std::string why = message_of(output);
                log::info("git {} in {} failed: {}", acting->name, repo.key, why);
                repo.action.reset();  // ends this callback's object: only locals after
                finish(false,
                       why.empty() ? i18n::trcf("git", "{} failed", acting->name)
                                   : i18n::trcf("git", "{} failed: {}", acting->name, why),
                       *acting);
                return;
            }
            step(repo, acting);
        });
    if (!repo.action->start(
            git_, arguments,
            {.merge_stderr = true, .environment = quiet_environment(options_.language)})) {
        repo.action.reset();
        finish(false, i18n::trcf("git", "{} failed", acting->name), *acting);
        return;
    }
    loop_.remove(repo.action_limit);
    repo.action_limit = loop_.add_timer(acting->limit, std::chrono::milliseconds{0}, [this, &repo] {
        repo.action_limit = 0;
        if (repo.action) {
            repo.action->stop();  // its end reports the failure
        }
    });
}

void Decorator::ask_commit(Repo& repo, Steps before, std::vector<std::string> paths) {
    if (!host_.ask_text) {
        return;
    }
    host_.ask_text(std::string{i18n::trc("git", "Commit")},
                   std::string{i18n::trc("git", "Commit message")}, std::string{},
                   std::string{i18n::trc("git", "Commit")},
                   [this, key = repo.key, before = std::move(before),
                    paths = std::move(paths)](const std::string& typed) -> std::string {
                       const std::string message = trimmed(typed);
                       if (message.empty()) {
                           return std::string{i18n::trc("git", "Enter a commit message")};
                       }
                       const auto it = repos_.find(key);
                       if (it == repos_.end()) {
                           return {};
                       }
                       Steps steps = before;
                       std::vector<std::string> commit{"commit", "--quiet", "-m", message};
                       if (!paths.empty()) {
                           commit.insert(commit.begin(), "--literal-pathspecs");
                           commit.emplace_back("--only");
                           commit.emplace_back("--");
                           commit.insert(commit.end(), paths.begin(), paths.end());
                       }
                       steps.push_back(std::move(commit));
                       act(*it->second, std::string{i18n::trc("git", "Commit")}, std::move(steps),
                           std::string{i18n::trc("git", "Committed")});
                       return {};
                   });
}

void Decorator::commit_all(Repo& repo) {
    if (!repo.status) {
        return;
    }
    const Status::Counts counts = repo.status->counts();
    if (counts.staged > 0) {
        ask_commit(repo, {}, {});
        return;
    }
    if (!host_.confirm) {
        return;
    }
    host_.confirm(std::string{i18n::trc("git", "Commit all changes?")},
                  std::string{i18n::trc("git", "Nothing is staged; new files are staged too.")},
                  std::string{i18n::trc("git", "Commit all")}, std::string{},
                  [this, key = repo.key](bool confirmed) {
                      const auto it = repos_.find(key);
                      if (confirmed && it != repos_.end()) {
                          ask_commit(*it->second, {{"add", "-A"}}, {});
                      }
                  });
}

void Decorator::discard(Repo& repo, const std::vector<std::string>& paths) {
    if (!repo.status || !host_.confirm) {
        return;
    }
    const bool has_head = !repo.status->branch.oid.empty();
    std::vector<std::string> restore;  // back to HEAD
    std::vector<std::string> unadd;    // new in the index: out of it, then to the trash
    std::vector<std::string> trash;    // untracked: to the trash
    for (const std::string& path : paths) {
        for (const FileStatus* file : statuses_at(*repo.status, path)) {
            switch (file->kind) {
                case FileStatus::Kind::ignored:
                    break;
                case FileStatus::Kind::untracked:
                    trash.push_back(file->path);
                    break;
                case FileStatus::Kind::conflict:
                case FileStatus::Kind::tracked:
                    if (!has_head || file->staged == 'A' || file->staged == 'R' ||
                        file->staged == 'C') {
                        unadd.push_back(file->path);
                        trash.push_back(file->path);
                        if (!file->original.empty() && has_head) {
                            restore.push_back(file->original);
                        }
                    } else {
                        restore.push_back(file->path);
                    }
                    break;
            }
        }
    }
    const std::size_t count = restore.size() + trash.size();
    if (count == 0) {
        return;
    }
    Steps steps;
    if (!unadd.empty()) {
        std::vector<std::string> arguments{
            "--literal-pathspecs", "rm", "--cached", "-q", "-f", "--"};
        arguments.insert(arguments.end(), unadd.begin(), unadd.end());
        steps.push_back(std::move(arguments));
    }
    if (!restore.empty()) {
        std::vector<std::string> arguments{
            "--literal-pathspecs", "restore", "--source=HEAD", "--staged", "--worktree", "--"};
        arguments.insert(arguments.end(), restore.begin(), restore.end());
        steps.push_back(std::move(arguments));
    }
    std::vector<std::filesystem::path> to_trash;
    for (const std::string& path : trash) {
        to_trash.push_back(repo.repository.worktree / path);
    }
    // Which files (the first few by name), and what happens to them in one short line.
    std::string names;
    std::size_t listed = 0;
    for (const auto* list : {&restore, &trash}) {
        for (const std::string& path : *list) {
            if (listed == 3) {
                break;
            }
            names.append(names.empty() ? "" : ", ")
                .append(std::filesystem::path{path}.filename().string());
            ++listed;
        }
    }
    if (count > listed) {
        names += i18n::trnc("git", " and {} more", " and {} more", count - listed);
    }
    host_.confirm(
        i18n::trnc("git", "Discard the changes of {} file?", "Discard the changes of {} files?",
                   count),
        std::move(names), std::string{i18n::trc("git", "Discard")},
        std::string{trash.empty()
                        ? i18n::trc("git", "Uncommitted changes are lost.")
                        : i18n::trc("git", "Changes are lost; new files go to the trash.")},
        [this, key = repo.key, steps = std::move(steps),
         to_trash = std::move(to_trash)](bool confirmed) {
            const auto it = repos_.find(key);
            if (!confirmed || it == repos_.end()) {
                return;
            }
            act(*it->second, std::string{i18n::trc("git", "Discard")}, steps,
                std::string{i18n::trc("git", "Changes discarded")}, [this, to_trash] {
                    if (!to_trash.empty() && host_.trash) {
                        host_.trash(to_trash);
                    }
                });
        });
}

void Decorator::ignore(Repo& repo, std::vector<std::string> paths) {
    if (repo.action) {
        return;
    }
    repo.action_name = std::string{i18n::trc("git", "Add to .gitignore")};
    ++repo.generation;
    // On a worker: the file is read and written, the tree may be remote.
    pool_.submit(
        core::Priority::visible,
        [worktree = repo.repository.worktree,
         paths = std::move(paths)](const core::CancelToken& /*token*/) -> std::string {
            const std::filesystem::path file = worktree / ".gitignore";
            std::string text;
            {
                std::ifstream in{file, std::ios::binary};
                text.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
            }
            std::vector<std::string> lines;
            std::istringstream in{text};
            for (std::string line; std::getline(in, line);) {
                lines.push_back(trimmed(line));
            }
            std::string added;
            for (const std::string& path : paths) {
                std::error_code error;
                std::string line = "/" + path;
                if (std::filesystem::is_directory(worktree / path, error)) {
                    line += "/";
                }
                if (std::ranges::find(lines, line) == lines.end()) {
                    lines.push_back(line);
                    added += line + "\n";
                }
            }
            if (added.empty()) {
                return {};
            }
            std::ofstream out{file, std::ios::binary | std::ios::app};
            if (!text.empty() && !text.ends_with('\n')) {
                out << '\n';
            }
            out << added;
            out.flush();
            return out ? std::string{}
                       : std::string{i18n::trc("git", "Could not write .gitignore")};
        },
        [this, alive = alive_, key = repo.key](const std::string& problem) {
            if (!*alive) {
                return;
            }
            const auto it = repos_.find(key);
            if (it == repos_.end()) {
                return;
            }
            Repo& done = *it->second;
            done.action_name.clear();
            ++done.generation;
            if (host_.message) {
                host_.message(problem.empty() ? std::string{i18n::trc("git", "Added to .gitignore")}
                                              : problem,
                              !problem.empty());
            }
            run(done);
        });
}

std::optional<views::MenuItem> Decorator::menu(const views::Folder& folder,
                                               const std::vector<std::string>& names) {
    Place* const place = place_of(folder);
    if (place == nullptr || place->repo == nullptr || !place->repo->status || place->repo->failed) {
        return std::nullopt;
    }
    Repo& repo = *place->repo;
    views::MenuItem git;
    git.id = "git";
    git.label = "Git";
    git.submenu = true;
    if (names.empty()) {
        git.children = repo_items(repo, true);
        return git;
    }

    std::vector<std::string> paths;
    for (const std::string& name : names) {
        paths.push_back(place->prefix + name);
    }
    git.children = path_items(repo, paths, place->ignored);
    return git;
}

std::vector<views::MenuItem> Decorator::path_items(Repo& repo,
                                                   const std::vector<std::string>& paths,
                                                   bool inside_ignored) {
    std::vector<views::MenuItem> items;
    const bool idle = !repo.action && repo.action_name.empty();
    const auto item = [&](std::string id, std::string_view label, bool enabled,
                          std::function<void()> action, bool separator = false) {
        views::MenuItem each;
        each.id = "git_" + id;
        each.label = std::string{label};
        each.enabled = enabled && idle;
        each.separator = separator;
        each.action = std::move(action);
        items.push_back(std::move(each));
    };
    const std::string key = repo.key;
    const auto with_repo = [this, key](auto then) {
        return [this, key, then] {
            if (const auto it = repos_.find(key); it != repos_.end()) {
                then(*it->second);
            }
        };
    };
    bool stageable = false;
    bool staged = false;
    bool changed = false;
    bool ignorable = false;
    for (const std::string& path : paths) {
        bool ignored_here = inside_ignored;
        for (const FileStatus* file : statuses_at(*repo.status, path)) {
            if (file->kind == FileStatus::Kind::ignored) {
                ignored_here = ignored_here || file->path == path || file->path == path + "/";
                continue;
            }
            changed = true;
            stageable =
                stageable || file->kind != FileStatus::Kind::tracked || file->unstaged != '.';
            staged = staged || (file->kind == FileStatus::Kind::tracked && file->staged != '.');
        }
        const std::string name = std::filesystem::path{path}.filename().string();
        ignorable = ignorable || (!ignored_here && name != ".gitignore");
    }
    const bool has_head = !repo.status->branch.oid.empty();
    const auto with_paths = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), "--literal-pathspecs");
        arguments.emplace_back("--");
        arguments.insert(arguments.end(), paths.begin(), paths.end());
        return arguments;
    };
    item("stage", i18n::trc("git", "Stage"), stageable,
         with_repo([this, steps = Steps{with_paths({"add", "-A"})}](Repo& at) {
             act(at, std::string{i18n::trc("git", "Stage")}, steps, {});
         }));
    item(
        "unstage", i18n::trc("git", "Unstage"), staged,
        with_repo([this, steps = Steps{has_head ? with_paths({"restore", "--staged"})
                                                : with_paths({"rm", "--cached", "-r", "-q"})}](
                      Repo& at) { act(at, std::string{i18n::trc("git", "Unstage")}, steps, {}); }));
    item("commit", i18n::trc("git", "Commit…"), changed,
         with_repo([this, add = Steps{with_paths({"add", "-A"})}, paths](Repo& at) {
             ask_commit(at, add, paths);
         }),
         true);
    item("diff", i18n::trc("git", "Show Changes"), changed, with_repo([this, paths](Repo& at) {
             if (at.status) {
                 show_diff(at.key, diff_items(*at.status, paths), 0);
             }
         }));
    item("discard", i18n::trc("git", "Discard Changes…"), changed,
         with_repo([this, paths](Repo& at) { discard(at, paths); }));
    item("ignore", i18n::trc("git", "Add to .gitignore"), ignorable,
         with_repo([this, paths](Repo& at) { ignore(at, paths); }), true);
    return items;
}

std::vector<Decorator::DiffItem> Decorator::diff_items(const Status& status,
                                                       const std::vector<std::string>& paths) {
    std::vector<DiffItem> items;
    for (const std::string& path : paths) {
        for (const FileStatus* file : statuses_at(status, path)) {
            if (file->kind == FileStatus::Kind::ignored) {
                continue;
            }
            DiffKind kind = DiffKind::changes;
            if (file->kind == FileStatus::Kind::untracked) {
                kind = DiffKind::untracked;
            } else if (file->kind == FileStatus::Kind::tracked && file->unstaged == '.') {
                kind = DiffKind::staged;
            }
            items.push_back({file->path, kind});
        }
    }
    return items;
}

void Decorator::show_diff(const std::string& key, std::vector<DiffItem> items, std::size_t index) {
    const auto it = repos_.find(key);
    if (it == repos_.end() || index >= items.size() || !host_.show_text) {
        return;
    }
    const DiffItem item = items[index];
    const std::string_view what = item.kind == DiffKind::staged ? i18n::trc("git", "Staged Changes")
                                  : item.kind == DiffKind::untracked ? i18n::trc("git", "Untracked")
                                                                     : i18n::trc("git", "Changes");
    const std::string title = item.path + std::string{kSeparator} + std::string{what};
    // The arrows: the next or previous file of the same list, none past its ends.
    auto step = [this, key, items, index](int delta) {
        const auto next = static_cast<std::ptrdiff_t>(index) + delta;
        if (next >= 0 && static_cast<std::size_t>(next) < items.size()) {
            show_diff(key, items, static_cast<std::size_t>(next));
        }
    };
    text::Text loading;
    loading.lines = {std::string{i18n::trc("git", "Loading…")}};
    host_.show_text(title, loading, step);
    const std::uint64_t generation = ++diff_generation_;
    diff_query_ = std::make_unique<platform::ProgramOutput>(
        loop_, [this, generation, title, step](const std::string& output, int wait_status) {
            if (generation != diff_generation_) {
                return;
            }
            // git diff --no-index exits 1 when the files differ: that is its answer, not a failure.
            const bool answered = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) <= 1;
            text::Text text = answered ? diff_text(output) : text::Text{};
            if (!answered) {
                text.lines = {i18n::trcf("git", "{} failed", i18n::trc("git", "Show Changes"))};
            } else if (text.lines.empty()) {
                text.lines = {std::string{i18n::trc("git", "No changes")}};
            }
            if (host_.show_text) {
                host_.show_text(title, std::move(text), step);
            }
            diff_query_.reset();  // ends this callback's object: nothing after
        });
    if (!diff_query_->start(
            git_, diff_arguments(it->second->repository.worktree, item.path, item.kind))) {
        diff_query_.reset();
    }
}

std::vector<views::MenuItem> Decorator::branch_items(Repo& repo) {
    std::vector<views::MenuItem> items;
    const bool idle = !repo.action && repo.action_name.empty();
    const std::string current = repo.status ? repo.status->branch.name : std::string{};
    const std::string key = repo.key;
    const auto switch_to = [this, key](std::string name, std::vector<std::string> arguments) {
        return [this, key, name = std::move(name), arguments = std::move(arguments)] {
            if (const auto it = repos_.find(key); it != repos_.end()) {
                act(*it->second, std::string{i18n::trc("git", "Switch Branch")}, {arguments},
                    i18n::trcf("git", "Switched to {}", name));
            }
        };
    };
    for (const std::string& name : repo.local_branches) {
        views::MenuItem item;
        item.id = "git_branch:" + name;
        item.label = name;
        item.radio = true;
        item.checked = name == current;
        item.enabled = idle && name != current;
        item.action = switch_to(name, {"switch", "--quiet", name});
        items.push_back(std::move(item));
    }
    // Remote branches without a local one: a local branch that tracks it.
    bool first_remote = true;
    for (const std::string& remote : repo.remote_branches) {
        const std::string name = remote.substr(remote.find('/') + 1);
        if (std::ranges::find(repo.local_branches, name) != repo.local_branches.end()) {
            continue;
        }
        views::MenuItem item;
        item.id = "git_branch:" + remote;
        item.label = remote;
        item.enabled = idle;
        item.separator = first_remote && !items.empty();
        item.action = switch_to(name, {"switch", "--quiet", "-c", name, "--track", remote});
        items.push_back(std::move(item));
        first_remote = false;
    }
    views::MenuItem create;
    create.id = "git_new_branch";
    create.label = std::string{i18n::trc("git", "New Branch…")};
    create.separator = !items.empty();
    create.enabled = idle;
    create.action = [this, key] {
        if (!host_.ask_text) {
            return;
        }
        host_.ask_text(std::string{i18n::trc("git", "New Branch")},
                       std::string{i18n::trc("git", "Branch name")}, std::string{},
                       std::string{i18n::trc("git", "Create")},
                       [this, key](const std::string& typed) -> std::string {
                           const std::string name = trimmed(typed);
                           if (name.empty() ||
                               name.find_first_of(" \t~^:?*[\\") != std::string::npos ||
                               name.starts_with('-')) {
                               return std::string{i18n::trc("git", "Not a valid branch name")};
                           }
                           if (const auto it = repos_.find(key); it != repos_.end()) {
                               act(*it->second, std::string{i18n::trc("git", "New Branch")},
                                   {{"switch", "--quiet", "-c", name}},
                                   i18n::trcf("git", "Switched to {}", name));
                           }
                           return {};
                       });
    };
    items.push_back(std::move(create));
    return items;
}

std::vector<std::string> Decorator::pull_arguments(const Repo& repo) {
    // Without pull.rebase or pull.ff, git refuses diverged branches: merge, as editors do.
    return repo.pull_configured
               ? std::vector<std::string>{"pull", "--no-edit", "--quiet"}
               : std::vector<std::string>{"pull", "--no-rebase", "--no-edit", "--quiet"};
}

std::vector<std::string> Decorator::push_arguments(const Repo& repo) {
    const bool upstream = repo.status && !repo.status->branch.upstream.empty();
    return upstream ? std::vector<std::string>{"push", "--quiet"}
                    : std::vector<std::string>{"push", "--quiet", "-u", "origin", "HEAD"};
}

void Decorator::sync(Repo& repo) {
    act(repo, std::string{i18n::trc("git", "Sync")}, {pull_arguments(repo), push_arguments(repo)},
        std::string{i18n::trc("git", "Synced")});
}

void Decorator::commit_typed(Repo& repo, const std::string& typed, CommitMode mode) {
    if (!repo.status) {
        return;
    }
    const std::string message = trimmed(typed);
    if (message.empty() && mode != CommitMode::amend) {
        if (host_.message) {
            host_.message(std::string{i18n::trc("git", "Enter a commit message")}, true);
        }
        return;
    }
    const Status::Counts counts = repo.status->counts();
    const bool nothing_staged = counts.staged == 0;
    const bool any = counts.changed + counts.untracked + counts.conflicts > 0;
    const auto run = [this, key = repo.key, message, mode](bool add_all) {
        const auto it = repos_.find(key);
        if (it == repos_.end()) {
            return;
        }
        Repo& at = *it->second;
        Steps steps;
        if (add_all) {
            steps.push_back({"add", "-A"});
        }
        std::vector<std::string> commit{"commit", "--quiet"};
        if (mode == CommitMode::amend) {
            commit.emplace_back("--amend");
        }
        if (message.empty()) {
            commit.emplace_back("--no-edit");
        } else {
            commit.emplace_back("-m");
            commit.push_back(message);
        }
        steps.push_back(std::move(commit));
        if (mode == CommitMode::sync) {
            steps.push_back(pull_arguments(at));
        }
        if (mode == CommitMode::push || mode == CommitMode::sync) {
            steps.push_back(push_arguments(at));
        }
        act(at, std::string{i18n::trc("git", "Commit")}, std::move(steps),
            std::string{i18n::trc("git", "Committed")}, [this, key] {
                if (const auto found = repos_.find(key); found != repos_.end()) {
                    ++found->second->input_revision;  // the message field empties
                    found->second->input_text.clear();
                }
            });
    };
    // Nothing staged: everything, after asking (an amend may change the message alone).
    if (nothing_staged && any && mode != CommitMode::amend && host_.confirm) {
        host_.confirm(std::string{i18n::trc("git", "Commit all changes?")},
                      std::string{i18n::trc("git", "Nothing is staged; new files are staged too.")},
                      std::string{i18n::trc("git", "Commit all")}, std::string{},
                      [run](bool confirmed) {
                          if (confirmed) {
                              run(true);
                          }
                      });
        return;
    }
    run(false);
}

std::vector<views::MenuItem> Decorator::repo_items(Repo& repo, bool with_commit) {
    std::vector<views::MenuItem> items;
    for (views::DetailSection::Button& button : buttons(repo)) {
        if (!with_commit && button.id == "commit") {
            continue;
        }
        views::MenuItem each;
        each.id = "git_" + button.id;
        each.label = button.label;
        each.enabled = button.enabled;
        each.separator = button.id == "fetch";
        each.action = std::move(button.action);
        items.push_back(std::move(each));
    }
    return items;
}

std::vector<views::DetailSection::Button> Decorator::buttons(Repo& repo) {
    const bool idle = !repo.action && repo.action_name.empty();
    const Status::Counts counts = repo.status ? repo.status->counts() : Status::Counts{};
    const bool any = counts.staged + counts.changed + counts.untracked + counts.conflicts > 0;
    const bool has_head = repo.status && !repo.status->branch.oid.empty();
    const bool upstream = repo.status && !repo.status->branch.upstream.empty();
    const std::string key = repo.key;
    const auto run_on = [this, key](auto then) {
        return [this, key, then] {
            if (const auto it = repos_.find(key); it != repos_.end()) {
                then(*it->second);
            }
        };
    };
    const auto simple = [&](std::string_view label, std::vector<std::string> arguments,
                            std::string_view done) {
        return run_on([this, name = std::string{label}, arguments = std::move(arguments),
                       done = std::string{done}](Repo& at) { act(at, name, {arguments}, done); });
    };
    std::vector<views::DetailSection::Button> result;
    result.push_back({"commit",
                      std::string{i18n::trc("git", "Commit…")},
                      idle && any,
                      run_on([this](Repo& at) { commit_all(at); }),
                      {},
                      {}});
    result.push_back({"stage_all",
                      std::string{i18n::trc("git", "Stage All")},
                      idle && any,
                      simple(i18n::trc("git", "Stage All"), {"add", "-A"}, {}),
                      {},
                      {}});
    result.push_back(
        {"unstage_all",
         std::string{i18n::trc("git", "Unstage All")},
         idle && counts.staged > 0,
         simple(i18n::trc("git", "Unstage All"),
                has_head ? std::vector<std::string>{"restore", "--staged", ":/"}
                         : std::vector<std::string>{"rm", "--cached", "-r", "-q", ":/"},
                {}),
         {},
         {}});
    result.push_back(
        {"fetch",
         std::string{i18n::trc("git", "Fetch")},
         idle,
         simple(i18n::trc("git", "Fetch"), {"fetch", "--quiet"}, i18n::trc("git", "Fetched")),
         {},
         {}});
    result.push_back(
        {"pull",
         std::string{i18n::trc("git", "Pull")},
         idle && upstream,
         simple(i18n::trc("git", "Pull"), pull_arguments(repo), i18n::trc("git", "Pulled")),
         {},
         {}});
    result.push_back(
        {"push",
         std::string{i18n::trc("git", "Push")},
         idle && has_head,
         simple(i18n::trc("git", "Push"), push_arguments(repo), i18n::trc("git", "Pushed")),
         {},
         {}});
    return result;
}

}  // namespace rummage::git
