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
        if (line.empty() || prefix == "hint" || prefix == "Hinweis") {
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
    const bool idle = !repo.action && repo.action_name.empty();
    views::MenuItem git;
    git.id = "git";
    git.label = "Git";
    git.submenu = true;
    const auto item = [&](std::string id, std::string_view label, bool enabled,
                          std::function<void()> action, bool separator = false) {
        views::MenuItem each;
        each.id = "git_" + id;
        each.label = std::string{label};
        each.enabled = enabled && idle;
        each.separator = separator;
        each.action = std::move(action);
        git.children.push_back(std::move(each));
    };
    const std::string key = repo.key;
    const auto with_repo = [this, key](auto then) {
        return [this, key, then] {
            if (const auto it = repos_.find(key); it != repos_.end()) {
                then(*it->second);
            }
        };
    };
    if (names.empty()) {
        for (views::DetailSection::Button& button : buttons(repo)) {
            item(button.id, button.label, button.enabled, std::move(button.action),
                 button.id == "fetch");
        }
        return git;
    }

    std::vector<std::string> paths;
    bool stageable = false;
    bool staged = false;
    bool changed = false;
    bool ignorable = false;
    for (const std::string& name : names) {
        std::string path = place->prefix + name;
        bool ignored_here = place->ignored;
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
        ignorable = ignorable || (!ignored_here && name != ".gitignore");
        paths.push_back(std::move(path));
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
    item("discard", i18n::trc("git", "Discard Changes…"), changed,
         with_repo([this, paths](Repo& at) { discard(at, paths); }));
    item("ignore", i18n::trc("git", "Add to .gitignore"), ignorable,
         with_repo([this, paths](Repo& at) { ignore(at, paths); }), true);
    return git;
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
    result.push_back({"commit", std::string{i18n::trc("git", "Commit…")}, idle && any,
                      run_on([this](Repo& at) { commit_all(at); })});
    result.push_back({"stage_all", std::string{i18n::trc("git", "Stage All")}, idle && any,
                      simple(i18n::trc("git", "Stage All"), {"add", "-A"}, {})});
    result.push_back(
        {"unstage_all", std::string{i18n::trc("git", "Unstage All")}, idle && counts.staged > 0,
         simple(i18n::trc("git", "Unstage All"),
                has_head ? std::vector<std::string>{"restore", "--staged", ":/"}
                         : std::vector<std::string>{"rm", "--cached", "-r", "-q", ":/"},
                {})});
    result.push_back(
        {"fetch", std::string{i18n::trc("git", "Fetch")}, idle,
         simple(i18n::trc("git", "Fetch"), {"fetch", "--quiet"}, i18n::trc("git", "Fetched"))});
    result.push_back({"pull", std::string{i18n::trc("git", "Pull")}, idle && upstream,
                      simple(i18n::trc("git", "Pull"), {"pull", "--no-edit", "--quiet"},
                             i18n::trc("git", "Pulled"))});
    result.push_back(
        {"push", std::string{i18n::trc("git", "Push")}, idle && has_head,
         simple(i18n::trc("git", "Push"),
                upstream ? std::vector<std::string>{"push", "--quiet"}
                         : std::vector<std::string>{"push", "--quiet", "-u", "origin", "HEAD"},
                i18n::trc("git", "Pushed"))});
    return result;
}

}  // namespace rummage::git
