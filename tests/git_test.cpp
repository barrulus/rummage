#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/git/decorator.hpp"
#include "rummage/git/repository.hpp"
#include "rummage/git/status.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/settings_dialog.hpp"
#include "temp_dir.hpp"

namespace git = rummage::git;
using git::FileStatus;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

std::string path_variable() {
    const char* const path = std::getenv("PATH");
    return path != nullptr ? path : "/usr/bin:/bin";
}

// git with a fixed identity and dates and without the user's or the system's configuration, in
// directory. Its standard output; the test fails when it does.
std::string run_git(const std::filesystem::path& directory, std::vector<std::string> arguments,
                    bool may_fail = false) {
    std::vector<std::string> words{"git",
                                   "-C",
                                   directory.string(),
                                   "-c",
                                   "init.defaultBranch=main",
                                   "-c",
                                   "protocol.file.allow=always"};
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> variables{"GIT_CONFIG_NOSYSTEM=1",
                                       "GIT_CONFIG_GLOBAL=/dev/null",
                                       "GIT_AUTHOR_NAME=Ada",
                                       "GIT_AUTHOR_EMAIL=ada@example.org",
                                       "GIT_COMMITTER_NAME=Ada",
                                       "GIT_COMMITTER_EMAIL=ada@example.org",
                                       "GIT_AUTHOR_DATE=@1700000000 +0000",
                                       "GIT_COMMITTER_DATE=@1700000000 +0000",
                                       "GIT_EDITOR=true",
                                       std::string{"PATH="} + path_variable(),
                                       "HOME=/nonexistent",
                                       "LC_ALL=C"};
    std::vector<char*> envp;
    for (std::string& variable : variables) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);

    std::array<int, 2> pipe_fds{-1, -1};
    REQUIRE(pipe(pipe_fds.data()) == 0);
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, "git", &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    REQUIRE(spawned == 0);
    std::string output;
    std::array<char, 4096> buffer{};
    for (ssize_t got = 0; (got = read(pipe_fds[0], buffer.data(), buffer.size())) > 0;) {
        output.append(buffer.data(), static_cast<std::size_t>(got));
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!may_fail) {
        INFO("git " << words[7]);
        REQUIRE((WIFEXITED(status) && WEXITSTATUS(status) == 0));
    }
    return output;
}

void write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream{file} << text;
}

// A repository at dir/name with one commit of a.txt ("one\n").
std::filesystem::path make_repository(const TempDir& dir, const std::string& name = "repo") {
    const std::filesystem::path repo = dir.path / name;
    std::filesystem::create_directories(repo);
    run_git(repo, {"init", "-q"});
    write(repo / "a.txt", "one\n");
    run_git(repo, {"add", "a.txt"});
    run_git(repo, {"commit", "-q", "-m", "First"});
    return repo;
}

git::Status status_of(const std::filesystem::path& repo, bool ignored = false) {
    std::vector<std::string> arguments = git::status_arguments(repo, ignored);
    arguments.erase(arguments.begin(), arguments.begin() + 3);  // run_git adds its own -C
    const std::optional<git::Status> status = git::parse_status(run_git(repo, arguments));
    REQUIRE(status.has_value());
    return *status;
}

const FileStatus* file_named(const git::Status& status, std::string_view path) {
    for (const FileStatus& file : status.files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("git: a folder outside any repository has none") {
    const TempDir dir;
    CHECK_FALSE(git::find_repository(dir.path).has_value());
    CHECK_FALSE(git::find_repository(dir.path / "missing").has_value());
}

TEST_CASE("git: the repository is found from its folders, not from inside .git") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    std::filesystem::create_directories(repo / "src" / "deep");

    const auto top = git::find_repository(repo);
    REQUIRE(top.has_value());
    CHECK(top->worktree == repo);
    CHECK(top->git_dir == repo / ".git");
    CHECK(top->common_dir == repo / ".git");
    CHECK(git::find_repository(repo / "src" / "deep/") == top);
    CHECK_FALSE(git::find_repository(repo / ".git" / "refs").has_value());
    CHECK(git::operation_of(*top) == git::Operation::none);
}

TEST_CASE("git: a clean working tree") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    const git::Status status = status_of(repo);
    CHECK(status.branch.name == "main");
    CHECK(status.branch.oid.size() == 40);
    CHECK(status.branch.upstream.empty());
    CHECK(status.files.empty());
    CHECK(status.counts() == git::Status::Counts{});

    std::vector<std::string> arguments = git::commit_arguments(repo);
    arguments.erase(arguments.begin(), arguments.begin() + 3);
    const auto commit = git::parse_commit(run_git(repo, arguments));
    REQUIRE(commit.has_value());
    CHECK(commit->hash.size() >= 7);
    CHECK(status.branch.oid.starts_with(commit->hash));
    CHECK(commit->subject == "First");
    CHECK(commit->author == "Ada");
    CHECK(commit->time == 1700000000);
}

TEST_CASE("git: a repository without commits") {
    const TempDir dir;
    const std::filesystem::path repo = dir.path / "empty";
    std::filesystem::create_directories(repo);
    run_git(repo, {"init", "-q"});
    write(repo / "new.txt", "x");
    const git::Status status = status_of(repo);
    CHECK(status.branch.name == "main");
    CHECK(status.branch.oid.empty());
    CHECK(status.counts().untracked == 1);
    std::vector<std::string> arguments = git::commit_arguments(repo);
    arguments.erase(arguments.begin(), arguments.begin() + 3);
    CHECK_FALSE(git::parse_commit(run_git(repo, arguments, true)).has_value());
}

TEST_CASE("git: modified, staged, untracked, ignored, renamed and deleted files") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "b.txt", "b\n");
    write(repo / "c.txt", "c\n");
    write(repo / ".gitignore", "build/\n*.log\n");
    run_git(repo, {"add", "b.txt", "c.txt", ".gitignore"});
    run_git(repo, {"commit", "-q", "-m", "More"});

    write(repo / "a.txt", "two\n");  // modified, not staged
    write(repo / "b.txt", "b2\n");   // staged, then changed again
    run_git(repo, {"add", "b.txt"});
    write(repo / "b.txt", "b3\n");
    run_git(repo, {"mv", "c.txt", "moved c.txt"});      // renamed, staged
    write(repo / "new file\nwith newline.txt", "n\n");  // untracked; -z keeps the name whole
    write(repo / "src" / "deep" / "x.txt", "x\n");      // untracked, in folders
    write(repo / "staged.txt", "s\n");
    run_git(repo, {"add", "staged.txt"});  // added
    write(repo / "build" / "out.o", "o");  // ignored folder
    write(repo / "debug.log", "l");        // ignored file

    const git::Status status = status_of(repo, true);
    const FileStatus* const a = file_named(status, "a.txt");
    REQUIRE(a != nullptr);
    CHECK(a->kind == FileStatus::Kind::tracked);
    CHECK(a->staged == '.');
    CHECK(a->unstaged == 'M');
    const FileStatus* const b = file_named(status, "b.txt");
    REQUIRE(b != nullptr);
    CHECK(b->staged == 'M');
    CHECK(b->unstaged == 'M');
    const FileStatus* const moved = file_named(status, "moved c.txt");
    REQUIRE(moved != nullptr);
    CHECK(moved->staged == 'R');
    CHECK(moved->original == "c.txt");
    const FileStatus* const added = file_named(status, "staged.txt");
    REQUIRE(added != nullptr);
    CHECK(added->staged == 'A');
    const FileStatus* const odd = file_named(status, "new file\nwith newline.txt");
    REQUIRE(odd != nullptr);
    CHECK(odd->kind == FileStatus::Kind::untracked);
    REQUIRE(file_named(status, "src/deep/x.txt") != nullptr);
    const FileStatus* const build = file_named(status, "build/");
    REQUIRE(build != nullptr);
    CHECK(build->kind == FileStatus::Kind::ignored);
    REQUIRE(file_named(status, "debug.log") != nullptr);

    const git::Status::Counts counts = status.counts();
    CHECK(counts.staged == 3);   // b, moved c, staged
    CHECK(counts.changed == 2);  // a, b
    CHECK(counts.untracked == 2);
    CHECK(counts.conflicts == 0);

    std::filesystem::remove(repo / "a.txt");
    const git::Status after = status_of(repo);
    const FileStatus* const deleted = file_named(after, "a.txt");
    REQUIRE(deleted != nullptr);
    CHECK(deleted->unstaged == 'D');
    CHECK(file_named(after, "debug.log") == nullptr);  // only listed when asked for
}

TEST_CASE("git: a merge with a conflict, and a rebase stopped by one") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    run_git(repo, {"switch", "-q", "-c", "other"});
    write(repo / "a.txt", "theirs\n");
    run_git(repo, {"commit", "-q", "-am", "Theirs"});
    run_git(repo, {"switch", "-q", "main"});
    write(repo / "a.txt", "ours\n");
    run_git(repo, {"commit", "-q", "-am", "Ours"});
    const auto repository = git::find_repository(repo);
    REQUIRE(repository.has_value());

    SUBCASE("merge") {
        run_git(repo, {"merge", "-q", "other"}, true);
        CHECK(git::operation_of(*repository) == git::Operation::merge);
        const git::Status status = status_of(repo);
        const FileStatus* const a = file_named(status, "a.txt");
        REQUIRE(a != nullptr);
        CHECK(a->kind == FileStatus::Kind::conflict);
        CHECK(a->staged == 'U');
        CHECK(a->unstaged == 'U');
        CHECK(status.counts().conflicts == 1);
    }
    SUBCASE("rebase") {
        run_git(repo, {"rebase", "-q", "other"}, true);
        CHECK(git::operation_of(*repository) == git::Operation::rebase);
        CHECK(status_of(repo).branch.name.empty());  // a rebase detaches HEAD
    }
}

TEST_CASE("git: a detached HEAD") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    run_git(repo, {"switch", "-q", "--detach"});
    const git::Status status = status_of(repo);
    CHECK(status.branch.name.empty());
    CHECK(status.branch.oid.size() == 40);
}

TEST_CASE("git: ahead of and behind the upstream") {
    const TempDir dir;
    const std::filesystem::path origin = make_repository(dir, "origin");
    run_git(dir.path, {"clone", "-q", origin.string(), "clone"});
    const std::filesystem::path clone = dir.path / "clone";
    write(clone / "mine.txt", "m\n");
    run_git(clone, {"add", "mine.txt"});
    run_git(clone, {"commit", "-q", "-m", "Mine"});
    write(origin / "theirs.txt", "t\n");
    run_git(origin, {"add", "theirs.txt"});
    run_git(origin, {"commit", "-q", "-m", "Theirs"});
    run_git(clone, {"fetch", "-q"});

    const git::Status status = status_of(clone);
    CHECK(status.branch.upstream == "origin/main");
    CHECK(status.branch.ahead == 1);
    CHECK(status.branch.behind == 1);
}

TEST_CASE("git: a linked worktree and a submodule") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);

    SUBCASE("worktree") {
        run_git(repo, {"worktree", "add", "-q", (dir.path / "wt").string()});
        std::filesystem::create_directories(dir.path / "wt" / "sub");
        const auto found = git::find_repository(dir.path / "wt" / "sub");
        REQUIRE(found.has_value());
        CHECK(found->worktree == dir.path / "wt");
        CHECK(found->git_dir == repo / ".git" / "worktrees" / "wt");
        CHECK(found->common_dir == repo / ".git");
        CHECK(status_of(dir.path / "wt").branch.name == "wt");
    }
    SUBCASE("submodule") {
        const std::filesystem::path library = make_repository(dir, "library");
        run_git(repo, {"submodule", "add", "-q", library.string(), "lib"});
        const auto found = git::find_repository(repo / "lib");
        REQUIRE(found.has_value());
        CHECK(found->worktree == repo / "lib");
        CHECK(found->git_dir == repo / ".git" / "modules" / "lib");
        CHECK(found->common_dir == found->git_dir);
        write(repo / "lib" / "a.txt", "changed\n");
        const git::Status super = status_of(repo);
        const FileStatus* const lib = file_named(super, "lib");
        REQUIRE(lib != nullptr);  // the superproject sees the submodule as modified
        CHECK(lib->unstaged == 'M');
    }
}

TEST_CASE("git: output that is not porcelain v2") {
    using namespace std::string_view_literals;
    CHECK_FALSE(git::parse_status("fatal: not a git repository"sv).has_value());
    CHECK_FALSE(git::parse_status("1 M"sv).has_value());
    CHECK_FALSE(git::parse_status("2 R. N... 100644 100644 100644 a b R100 new"sv).has_value());
    CHECK_FALSE(git::parse_status("# branch.ab +x -1"sv).has_value());
    CHECK_FALSE(git::parse_status("? "sv).has_value());
    CHECK(git::parse_status(""sv).has_value());
    const auto status =
        git::parse_status("# branch.oid (initial)\0# branch.head (detached)\0? a\0"sv);
    REQUIRE(status.has_value());
    CHECK(status->branch.oid.empty());
    CHECK(status->branch.name.empty());
    CHECK_FALSE(git::parse_commit("abc\0subject"sv).has_value());
    CHECK_FALSE(git::parse_commit("abc\0s\0a\0soon"sv).has_value());
}

namespace {

// Runs the loop until done() holds or 5 s pass.
bool run_until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(2ms, 2ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

void run_for(rummage::core::EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&loop] { loop.quit(0); });
    loop.run();
}

std::filesystem::path git_program() {
    const auto found = rummage::platform::find_program("git", path_variable());
    REQUIRE(found.has_value());
    return *found;
}

// A folder shown with the git decorator, as the browser would draw it.
struct Shown {
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    git::Decorator decorator;
    std::unique_ptr<rummage::views::Folder> folder;

    explicit Shown(const std::filesystem::path& directory,
                   std::filesystem::path program = git_program(), git::Options options = {})
        : decorator{loop, pool, watcher, std::move(program), [] {}, options} {
        rummage::i18n::set_language("en");
        folder = std::make_unique<rummage::views::Folder>(
            loop, pool, watcher, rummage::fs::MimeDatabase::load(), directory, [] {});
        REQUIRE(run_until(loop, [this] { return !folder->model().loading(); }));
    }

    // What the views would ask for each entry and the status bar for the folder.
    const rummage::views::StatusPart* draw() {
        for (const auto& entry : folder->entries()) {
            (void)decorator.badge(*folder, entry);
        }
        return decorator.status(*folder);
    }

    std::string text() {
        const rummage::views::StatusPart* const part = draw();
        return part != nullptr ? part->lead + part->text : std::string{};
    }

    const rummage::views::Badge* badge(std::string_view name) {
        for (const auto& entry : folder->entries()) {
            if (entry.name == name) {
                return decorator.badge(*folder, entry);
            }
        }
        FAIL("no entry " << name);
        return nullptr;
    }

    bool hides(std::string_view name) {
        for (const auto& entry : folder->entries()) {
            if (entry.name == name) {
                return decorator.hides(folder->path(), entry);
            }
        }
        return false;
    }

    bool wait_for(const std::function<bool()>& done) {
        return run_until(loop, [&] {
            draw();
            return done();
        });
    }
};

}  // namespace

TEST_CASE("git decorator: the status bar part and the badges of a working tree") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "a.txt", "two\n");
    write(repo / "sub" / "deep" / "new.txt", "n\n");
    write(repo / ".gitignore", "*.log\nbuild/\n");
    write(repo / "debug.log", "l");
    write(repo / "build" / "out.o", "o");
    Shown shown{repo};
    REQUIRE(shown.wait_for([&] { return shown.text().starts_with("main"); }));
    CHECK(shown.text() == "main  ·  1 changed  ·  2 untracked");

    const rummage::views::Badge* const a = shown.badge("a.txt");
    REQUIRE(a != nullptr);
    CHECK(a->text == "M");
    CHECK_FALSE(a->strong);
    const rummage::views::Badge* const sub = shown.badge("sub");
    REQUIRE(sub != nullptr);
    CHECK(sub->text == "•");
    CHECK(sub->tone == rummage::views::Badge::Tone::added);
    const rummage::views::Badge* const log = shown.badge("debug.log");
    REQUIRE(log != nullptr);
    CHECK(log->faded);
    REQUIRE(shown.badge("build") != nullptr);
    CHECK(shown.badge("build")->faded);
    CHECK(shown.badge(".gitignore") != nullptr);  // untracked too

    // The last commit, while the pointer is on the part.
    REQUIRE(shown.wait_for([&] {
        const rummage::views::StatusPart* const now = shown.draw();
        return now != nullptr && !now->detail.empty();
    }));
    const std::string detail = shown.draw()->detail;
    CHECK(detail.find(" First  ·  Ada  ·  ") != std::string::npos);
    CHECK(detail.ends_with(" ago"));

    // The information pane's section says the same, for good.
    const rummage::views::DetailSection* const section = shown.decorator.details(*shown.folder);
    REQUIRE(section != nullptr);
    CHECK(section->title == "GIT");
    const auto value_of = [&](std::string_view label) {
        for (const auto& row : section->rows) {
            if (row.label == label) {
                return row.value;
            }
        }
        return std::string{};
    };
    CHECK(value_of("Last commit").ends_with(" First"));
    CHECK(value_of("Author") == "Ada");

    // The cursor's entry says what its badge means.
    shown.folder->select_name("a.txt");
    CHECK(shown.text().ends_with("  ·  Modified, not staged"));

    SUBCASE("staging is seen through the index, and the badge becomes strong") {
        run_git(repo, {"add", "a.txt"});
        REQUIRE(shown.wait_for([&] {
            const rummage::views::Badge* const now = shown.badge("a.txt");
            return now != nullptr && now->strong;
        }));
        CHECK(shown.text().starts_with("main  ·  1 staged  ·  2 untracked"));
    }
    SUBCASE("a burst of new files is read once or twice, not once per file") {
        const std::size_t before = shown.decorator.queries();
        for (int i = 0; i < 50; ++i) {
            write(repo / ("burst" + std::to_string(i) + ".txt"), "b");
        }
        REQUIRE(
            shown.wait_for([&] { return shown.text().find("52 untracked") != std::string::npos; }));
        run_for(shown.loop, 500ms);
        shown.draw();
        CHECK(shown.decorator.queries() - before <= 2);
    }
    SUBCASE("switching the branch is seen through HEAD") {
        run_git(repo, {"switch", "-q", "-c", "feature"});
        REQUIRE(shown.wait_for([&] { return shown.text().starts_with("feature"); }));
    }
    SUBCASE("changes that keep coming are still read, at the latest 2 s after the first") {
        const std::size_t before = shown.decorator.queries();
        for (int i = 0; i < 30; ++i) {  // 3 s, one file every 100 ms
            write(repo / ("steady" + std::to_string(i) + ".txt"), "s");
            run_for(shown.loop, 100ms);
            shown.draw();
        }
        CHECK(shown.decorator.queries() - before >= 1);
        CHECK(shown.decorator.queries() - before <= 3);
        REQUIRE(
            shown.wait_for([&] { return shown.text().find("32 untracked") != std::string::npos; }));
    }
    SUBCASE("a commit on a branch in a folder is seen through the reflog") {
        run_git(repo, {"switch", "-q", "-c", "feature/deep"});
        REQUIRE(shown.wait_for([&] { return shown.text().starts_with("feature/deep"); }));
        run_git(repo, {"add", "a.txt"});
        REQUIRE(shown.wait_for([&] { return shown.text().find("1 staged") != std::string::npos; }));
        run_git(repo, {"commit", "-q", "-m", "Deep"});
        REQUIRE(shown.wait_for([&] {
            const rummage::views::StatusPart* const now = shown.draw();
            return now != nullptr && now->detail.find(" Deep  ·  ") != std::string::npos;
        }));
    }
    SUBCASE("a commit changes the last commit") {
        run_git(repo, {"commit", "-q", "-am", "Second"});
        REQUIRE(shown.wait_for([&] {
            const rummage::views::StatusPart* const now = shown.draw();
            return now != nullptr && now->detail.find("Second") != std::string::npos;
        }));
        CHECK(shown.badge("a.txt") == nullptr);
    }
    SUBCASE("options: ignored shown plainly or hidden, folders without a summary") {
        shown.decorator.set_options({.ignored = git::Options::Ignored::hide,
                                     .folder_status = false,
                                     .language = {},
                                     .commit_action = git::Options::CommitAction::commit});
        CHECK(shown.hides("debug.log"));
        CHECK_FALSE(shown.hides("a.txt"));
        CHECK(shown.badge("sub") == nullptr);
        shown.decorator.set_options({.ignored = git::Options::Ignored::show,
                                     .folder_status = true,
                                     .language = {},
                                     .commit_action = git::Options::CommitAction::commit});
        REQUIRE(shown.wait_for([&] { return shown.badge("sub") != nullptr; }));
        CHECK(shown.badge("debug.log") == nullptr);
        CHECK_FALSE(shown.hides("debug.log"));
    }
}

TEST_CASE("git decorator: hidden ignored entries leave the listing, and come back") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / ".gitignore", "*.log\n");
    write(repo / "debug.log", "l");
    Shown shown{repo,
                git_program(),
                {.ignored = git::Options::Ignored::hide,
                 .folder_status = true,
                 .language = {},
                 .commit_action = git::Options::CommitAction::commit}};
    const auto listed = [&](std::string_view name) {
        return std::ranges::any_of(shown.folder->entries(),
                                   [&](const auto& entry) { return entry.name == name; });
    };
    const auto refilter = [&] {
        shown.folder->set_entry_filter(
            rummage::views::hiding({}, &shown.decorator, shown.folder->path()));
    };
    shown.decorator.set_refilter_handler(refilter);
    refilter();
    REQUIRE(shown.wait_for([&] { return !listed("debug.log"); }));
    CHECK(listed("a.txt"));

    shown.decorator.set_options({.ignored = git::Options::Ignored::dim,
                                 .folder_status = true,
                                 .language = {},
                                 .commit_action = git::Options::CommitAction::commit});
    REQUIRE(shown.wait_for([&] { return listed("debug.log"); }));
    CHECK(shown.badge("debug.log")->faded);
}

TEST_CASE("git decorator: inside an ignored folder everything is faded") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / ".gitignore", "build/\n");
    write(repo / "build" / "out.o", "o");
    Shown shown{repo / "build"};
    REQUIRE(shown.wait_for([&] { return !shown.text().empty(); }));
    REQUIRE(shown.wait_for([&] { return shown.badge("out.o") != nullptr; }));
    CHECK(shown.badge("out.o")->faded);
}

TEST_CASE("git decorator: nothing outside a repository, nothing when git fails") {
    const TempDir dir;
    SUBCASE("no repository: no part, no badge, no git run") {
        write(dir.path / "a.txt", "a");
        Shown shown{dir.path};
        shown.draw();
        run_for(shown.loop, 200ms);
        CHECK(shown.draw() == nullptr);
        CHECK(shown.badge("a.txt") == nullptr);
        CHECK(shown.decorator.queries() == 0);
    }
    SUBCASE("git cannot be started") {
        const std::filesystem::path repo = make_repository(dir);
        Shown shown{repo, dir.path / "no-git-here"};
        shown.draw();
        run_for(shown.loop, 200ms);
        CHECK(shown.draw() == nullptr);
        CHECK(shown.badge("a.txt") == nullptr);
    }
    SUBCASE("git refuses: no part") {
        const std::filesystem::path repo = make_repository(dir);
        const std::filesystem::path fake = dir.write("git", "#!/bin/sh\necho nope >&2\nexit 128\n");
        std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
        Shown shown{repo, fake};
        shown.draw();
        run_for(shown.loop, 300ms);
        CHECK(shown.draw() == nullptr);
        CHECK(shown.decorator.queries() == 1);
    }
}

TEST_CASE("git decorator: a status that takes too long is stopped and paused") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    std::filesystem::create_directories(repo / "other");
    const std::filesystem::path fake = dir.write("git", "#!/bin/sh\nexec sleep 10\n");
    std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
    Shown shown{repo, fake};
    shown.decorator.set_time_limit(100ms);
    REQUIRE(shown.wait_for([&] { return shown.text() == "Git: paused"; }));
    // Changes while paused start nothing.
    write(repo / "new.txt", "n");
    run_for(shown.loop, 600ms);
    shown.draw();
    CHECK(shown.decorator.queries() == 1);
    CHECK(shown.text() == "Git: paused");
}

TEST_CASE("git settings: the dialog lists them and saves a new [git] table") {
    const TempDir dir;
    const std::filesystem::path path =
        dir.write("config.toml", "# mine\n[icon]\ntheme = 'system'\n");
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::views::SettingsDialog dialog{pool, [] {}};
    int applied = 0;
    dialog.open(path, [] {}, [&](const rummage::core::ConfigResult&) { ++applied; });
    REQUIRE(run_until(loop, [&] { return !dialog.busy(); }));
    REQUIRE(dialog.error().empty());
    REQUIRE(dialog.edit("git", "ignored", "hide"));
    REQUIRE(dialog.edit("git", "folder_status", "false"));
    REQUIRE(dialog.edit("git", "language", "en"));
    dialog.save();
    REQUIRE(run_until(loop, [&] { return !dialog.busy(); }));
    REQUIRE_MESSAGE(dialog.error().empty(), dialog.error());
    CHECK(applied == 1);
    const rummage::core::Config saved = rummage::core::load_config(path).config;
    CHECK(saved.git.enabled);
    CHECK(saved.git.ignored == "hide");
    CHECK_FALSE(saved.git.folder_status);
    CHECK(saved.git.language == "en");
    CHECK(saved.icon.theme == "system");
}

namespace {

// git run by the decorator commits as a fixed person, without the user's configuration.
struct GitIdentity {
    static constexpr std::array<std::pair<const char*, const char*>, 6> kVariables{{
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", "/dev/null"},
        {"GIT_AUTHOR_NAME", "Ada"},
        {"GIT_AUTHOR_EMAIL", "ada@example.org"},
        {"GIT_COMMITTER_NAME", "Ada"},
        {"GIT_COMMITTER_EMAIL", "ada@example.org"},
    }};

    GitIdentity() {
        for (const auto& [name, value] : kVariables) {
            setenv(name, value, 1);
        }
    }

    ~GitIdentity() {
        for (const auto& [name, value] : kVariables) {
            unsetenv(name);
        }
    }

    GitIdentity(const GitIdentity&) = delete;
    GitIdentity& operator=(const GitIdentity&) = delete;
    GitIdentity(GitIdentity&&) = delete;
    GitIdentity& operator=(GitIdentity&&) = delete;
};

// The browser's side, answered by the test.
struct TestHost {
    std::vector<std::pair<std::string, bool>> messages;
    std::string message_to_type = "A message";
    std::string refused;  // what accept said about the typed text
    bool confirm_answer = true;
    std::vector<std::filesystem::path> trashed;
    std::vector<std::string> actions;  // the buttons of the text cards asked for
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> revealed;

    struct Shown {
        std::string title;
        rummage::text::Text text;
        std::function<void(int)> step;
    };

    std::vector<Shown> shown;  // texts shown in Quick Look
    std::vector<std::filesystem::path> opened;

    rummage::views::DecorationHost host() {
        return {
            .message = [this](std::string text,
                              bool error) { messages.emplace_back(std::move(text), error); },
            .ask_text =
                [this](std::string /*title*/, std::string /*detail*/, std::string /*initial*/,
                       std::string action, std::function<std::string(const std::string&)> accept) {
                    actions.push_back(std::move(action));
                    refused = accept(message_to_type);
                },
            .confirm = [this](std::string /*title*/, std::string /*detail*/, std::string /*action*/,
                              std::string /*warning*/,
                              std::function<void(bool)> answer) { answer(confirm_answer); },
            .trash =
                [this](std::vector<std::filesystem::path> paths) {
                    for (const auto& path : paths) {
                        std::filesystem::remove_all(path);
                        trashed.push_back(path);
                    }
                },
            .show_text =
                [this](std::string title, rummage::text::Text text, std::function<void(int)> step) {
                    shown.push_back({std::move(title), std::move(text), std::move(step)});
                },
            .open_file = [this](const std::filesystem::path& file) { opened.push_back(file); },
            .reveal =
                [this](const std::filesystem::path& root, const std::filesystem::path& target) {
                    revealed.emplace_back(root, target);
                }};
    }
};

// Runs the git menu's item id for names (empty: the background menu); false when not there or
// not enabled.
bool choose(Shown& shown, const std::vector<std::string>& names, std::string_view id) {
    const auto menu = shown.decorator.menu(*shown.folder, names);
    REQUIRE(menu.has_value());
    for (const auto& item : menu->children) {
        if (item.id == id) {
            if (!item.enabled) {
                return false;
            }
            item.action();
            return true;
        }
    }
    FAIL("no menu item " << id);
    return false;
}

bool enabled(Shown& shown, const std::vector<std::string>& names, std::string_view id) {
    const auto menu = shown.decorator.menu(*shown.folder, names);
    REQUIRE(menu.has_value());
    for (const auto& item : menu->children) {
        if (item.id == id) {
            return item.enabled;
        }
    }
    return false;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in{file};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("git actions: stage, unstage, commit, ignore and discard from the menu") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "a.txt", "two\n");
    write(repo / "b.txt", "b\n");
    write(repo / "d" / "x.txt", "x\n");
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    REQUIRE(shown.wait_for([&] { return shown.badge("a.txt") != nullptr; }));
    const auto idle_and = [&](const std::function<bool()>& done) {
        return shown.wait_for([&] { return !shown.decorator.acting() && done(); });
    };

    CHECK_FALSE(enabled(shown, {"a.txt"}, "git_unstage"));
    REQUIRE(choose(shown, {"a.txt"}, "git_stage"));
    // One command at a time: a second waits for nothing and says why.
    CHECK_FALSE(choose(shown, {"b.txt"}, "git_stage"));
    REQUIRE(
        idle_and([&] { return shown.badge("a.txt") != nullptr && shown.badge("a.txt")->strong; }));
    REQUIRE(choose(shown, {"a.txt"}, "git_unstage"));
    REQUIRE(
        idle_and([&] { return shown.badge("a.txt") != nullptr && !shown.badge("a.txt")->strong; }));

    SUBCASE("commit the selection, an untracked file included") {
        host.message_to_type = "  ";
        REQUIRE(choose(shown, {"b.txt"}, "git_commit"));
        CHECK(host.refused == "Enter a commit message");
        CHECK(host.actions.back() == "Commit");
        host.message_to_type = "Add b";
        REQUIRE(choose(shown, {"b.txt"}, "git_commit"));
        CHECK(host.refused.empty());
        REQUIRE(idle_and([&] { return shown.badge("b.txt") == nullptr; }));
        CHECK(shown.badge("a.txt") != nullptr);  // not part of it
        CHECK(run_git(repo, {"log", "-1", "--format=%s"}) == "Add b\n");
        CHECK(host.messages.back() == std::pair<std::string, bool>{"Committed", false});
    }
    SUBCASE("ignore a folder, once") {
        REQUIRE(choose(shown, {"d"}, "git_ignore"));
        REQUIRE(idle_and([&] { return shown.badge("d") != nullptr && shown.badge("d")->faded; }));
        CHECK(read_file(repo / ".gitignore") == "/d/\n");
        CHECK_FALSE(enabled(shown, {"d"}, "git_ignore"));
    }
    SUBCASE("discard: tracked back to the commit, new files to the trash") {
        REQUIRE(choose(shown, {"a.txt", "b.txt"}, "git_discard"));
        REQUIRE(
            idle_and([&] { return shown.badge("a.txt") == nullptr && host.trashed.size() == 1; }));
        CHECK(read_file(repo / "a.txt") == "one\n");
        CHECK(host.trashed.front() == repo / "b.txt");
        CHECK_FALSE(enabled(shown, {"a.txt"}, "git_stage"));
    }
    SUBCASE("discard asks first") {
        host.confirm_answer = false;
        REQUIRE(choose(shown, {"a.txt"}, "git_discard"));
        run_for(shown.loop, 200ms);
        CHECK(read_file(repo / "a.txt") == "two\n");
    }
    SUBCASE("commit everything when nothing is staged, after asking") {
        host.message_to_type = "Everything";
        REQUIRE(choose(shown, {}, "git_commit"));
        REQUIRE(idle_and([&] { return shown.text().starts_with("main ✓"); }));
        CHECK(run_git(repo, {"log", "-1", "--format=%s"}) == "Everything\n");
    }
}

TEST_CASE("git actions: fetch, pull and push with a local remote") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path remote = dir.path / "remote.git";
    std::filesystem::create_directories(remote);
    run_git(remote, {"init", "-q", "--bare"});
    const std::filesystem::path repo = make_repository(dir);
    run_git(repo, {"remote", "add", "origin", remote.string()});
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    REQUIRE(shown.wait_for([&] { return shown.text().starts_with("main"); }));
    const auto idle_and = [&](const std::function<bool()>& done) {
        return shown.wait_for([&] { return !shown.decorator.acting() && done(); });
    };
    CHECK_FALSE(enabled(shown, {}, "git_pull"));  // no upstream yet

    // The first push sets the upstream.
    REQUIRE(choose(shown, {}, "git_push"));
    const auto upstream = [&] {
        const auto* section = shown.decorator.details(*shown.folder);
        if (section != nullptr) {
            for (const auto& row : section->rows) {
                if (row.label == "Upstream") {
                    return row.value;
                }
            }
        }
        return std::string{};
    };
    REQUIRE(idle_and([&] { return upstream() == "origin/main  ↑0 ↓0"; }));
    CHECK(host.messages.back() == std::pair<std::string, bool>{"Pushed", false});

    // Someone else pushes; fetch shows it behind, pull brings it.
    run_git(dir.path, {"clone", "-q", remote.string(), "other"});
    write(dir.path / "other" / "theirs.txt", "t\n");
    run_git(dir.path / "other", {"add", "theirs.txt"});
    run_git(dir.path / "other", {"commit", "-q", "-m", "Theirs"});
    run_git(dir.path / "other", {"push", "-q"});
    REQUIRE(choose(shown, {}, "git_fetch"));
    REQUIRE(idle_and([&] { return shown.text().starts_with("main ↓1"); }));
    REQUIRE(choose(shown, {}, "git_pull"));
    REQUIRE(idle_and([&] {
        return std::filesystem::exists(repo / "theirs.txt") && shown.text().starts_with("main ✓");
    }));

    // A remote that is not there: git's own words, no hang.
    run_git(repo, {"remote", "set-url", "origin", (dir.path / "gone.git").string()});
    REQUIRE(choose(shown, {}, "git_fetch"));
    REQUIRE(idle_and([&] { return !host.messages.empty() && host.messages.back().second; }));
    CHECK(host.messages.back().first.starts_with("Fetch failed: "));
    CHECK(host.messages.back().first.find("does not appear to be a git repository") !=
          std::string::npos);
}

TEST_CASE("git decorator: sorting by status, again when it changes") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "b.txt", "b\n");
    run_git(repo, {"add", "b.txt"});
    run_git(repo, {"commit", "-q", "-m", "b"});
    write(repo / ".gitignore", "*.log\n");
    write(repo / "a.txt", "changed\n");  // modified
    write(repo / "c.txt", "new\n");      // untracked
    write(repo / "d.log", "l");          // ignored
    Shown shown{repo};
    const auto apply = [&] {
        shown.folder->set_ranks(rummage::views::ranking(&shown.decorator, shown.folder->path()));
    };
    shown.decorator.set_refilter_handler(apply);
    apply();
    shown.folder->set_order({.key = rummage::fs::SortKey::mark, .directories_first = false});
    const auto names = [&] {
        std::vector<std::string> listed;
        for (const auto& entry : shown.folder->entries()) {
            listed.push_back(entry.name);
        }
        return listed;
    };
    // Changed, new (by name), unchanged (.git too), ignored.
    const std::vector<std::string> expected{"a.txt", ".gitignore", "c.txt",
                                            ".git",  "b.txt",      "d.log"};
    REQUIRE(shown.wait_for([&] { return names() == expected; }));
    run_git(repo, {"add", "-A"});
    run_git(repo, {"commit", "-q", "-m", "all"});
    const std::vector<std::string> clean{".git", ".gitignore", "a.txt", "b.txt", "c.txt", "d.log"};
    REQUIRE(shown.wait_for([&] { return names() == clean; }));
}

TEST_CASE("git decorator: never in a trash, even one inside a repository") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    for (const std::filesystem::path& trash :
         {repo / "data" / "Trash" / "files", repo / ".Trash-1000" / "files", repo / ".Trash"}) {
        write(trash / "gone.txt", "g\n");
        Shown shown{trash};
        shown.draw();
        run_for(shown.loop, 300ms);
        CHECK(shown.draw() == nullptr);
        CHECK(shown.badge("gone.txt") == nullptr);
        CHECK(shown.decorator.details(*shown.folder) == nullptr);
        CHECK_FALSE(shown.decorator.menu(*shown.folder, {"gone.txt"}).has_value());
        CHECK(shown.decorator.queries() == 0);
    }
    // The repository around it still has its status.
    Shown top{repo};
    CHECK(top.wait_for([&] { return top.text().starts_with("main"); }));
}

TEST_CASE("git language: git's texts apart from the interface's") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "a.txt", "two\n");
    Shown shown{repo};
    rummage::i18n::set_language("de");
    REQUIRE(shown.wait_for([&] { return shown.text() == "main  ·  1 geändert"; }));
    CHECK(shown.decorator.menu(*shown.folder, {"a.txt"})->children.front().label ==
          "Zum Commit vormerken");

    rummage::i18n::set_context_language("git", "en");
    shown.decorator.set_options({.ignored = git::Options::Ignored::dim,
                                 .folder_status = true,
                                 .language = "en",
                                 .commit_action = git::Options::CommitAction::commit});
    CHECK(shown.text() == "main  ·  1 changed");
    CHECK(shown.decorator.menu(*shown.folder, {"a.txt"})->children.front().label == "Stage");
    CHECK(rummage::i18n::tr("Delete") != "Delete");  // the interface stays German

    rummage::i18n::set_context_language("git", "");
    shown.decorator.set_options({.ignored = git::Options::Ignored::dim,
                                 .folder_status = true,
                                 .language = {},
                                 .commit_action = git::Options::CommitAction::commit});
    CHECK(shown.text() == "main  ·  1 geändert");
    rummage::i18n::set_language("en");
}

TEST_CASE("git: a remote as shown, a link only for https and the big hosts") {
    using View = git::RemoteView;
    CHECK(git::view_of_remote("git@github.com:barrulus/rummage.git") ==
          View{"github.com/barrulus/rummage", "https://github.com/barrulus/rummage"});
    CHECK(git::view_of_remote("ssh://git@codeberg.org/a/b.git") ==
          View{"codeberg.org/a/b", "https://codeberg.org/a/b"});
    CHECK(git::view_of_remote("https://user:secret-token@forge.example.org/group/repo.git/") ==
          View{"forge.example.org/group/repo", "https://forge.example.org/group/repo"});
    // An own server over ssh: shown, no link (its web page may be anywhere).
    CHECK(git::view_of_remote("ssh://git@192.168.20.1:2222/dquolke/repo") ==
          View{"ssh://192.168.20.1:2222/dquolke/repo", std::nullopt});
    CHECK(git::view_of_remote("git@forge.example.org:a/b.git") ==
          View{"git@forge.example.org:a/b.git", std::nullopt});
    CHECK(git::view_of_remote("http://user:pw@forge.example.org/a/b") ==
          View{"http://forge.example.org/a/b", std::nullopt});
    CHECK(git::view_of_remote("file:///srv/git/repo.git") ==
          View{"file:///srv/git/repo.git", std::nullopt});
    CHECK(git::view_of_remote("/srv/git/repo.git") == View{"/srv/git/repo.git", std::nullopt});
}

TEST_CASE("git decorator: the repository link in the information pane") {
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    run_git(repo, {"remote", "add", "origin", "git@github.com:someone/project.git"});
    Shown shown{repo};
    const auto link = [&]() -> const rummage::views::DetailSection::Row* {
        const auto* section = shown.decorator.details(*shown.folder);
        if (section == nullptr) {
            return nullptr;
        }
        for (const auto& row : section->rows) {
            if (row.label == "Repository") {
                return &row;
            }
        }
        return nullptr;
    };
    REQUIRE(shown.wait_for([&] { return link() != nullptr; }));
    const auto value_of = [&] {
        const rummage::views::DetailSection::Row* const row = link();
        return row != nullptr ? row->value : std::string{};
    };
    CHECK(value_of() == "github.com/someone/project");
    const rummage::views::DetailSection::Row* const row = link();
    REQUIRE((row != nullptr && row->action != nullptr));
    // A click runs xdg-open with the page (here a stand-in that writes down what it got).
    const std::filesystem::path bin = dir.path / "bin";
    std::filesystem::create_directories(bin);
    const std::filesystem::path got = dir.path / "opened.txt";
    write(bin / "xdg-open", "#!/bin/sh\necho \"$1\" > '" + got.string() + "'\n");
    std::filesystem::permissions(bin / "xdg-open", std::filesystem::perms::owner_all);
    const std::string old_path = path_variable();
    setenv("PATH", (bin.string() + ":" + old_path).c_str(), 1);
    row->action();
    const bool opened =
        shown.wait_for([&] { return read_file(got) == "https://github.com/someone/project\n"; });
    setenv("PATH", old_path.c_str(), 1);
    CHECK(opened);
    // A changed remote is read again through .git/config.
    run_git(repo, {"remote", "set-url", "origin", "https://codeberg.org/other/thing.git"});
    REQUIRE(shown.wait_for([&] { return value_of() == "codeberg.org/other/thing"; }));
}

TEST_CASE("git: the message of what went wrong, in any language git speaks") {
    CHECK(git::message_of("fatal: 'x' does not appear to be a git repository\nfatal: Could not "
                          "read from remote repository.\n\nPlease make sure\n") ==
          "'x' does not appear to be a git repository");
    CHECK(git::message_of("Schwerwiegend: Konnte nicht vom Remote-Repository lesen.\n\nBitte "
                          "stellen Sie sicher\n") == "Konnte nicht vom Remote-Repository lesen.");
    CHECK(git::message_of("hint: Updates were rejected\nerror: failed to push some refs\n") ==
          "failed to push some refs");
    CHECK(git::message_of("Everything up-to-date\n") == "Everything up-to-date");
    CHECK(git::message_of("hint: You have divergent branches\nhint:\nfatal: Need to specify how "
                          "to reconcile divergent branches.\n") ==
          "Need to specify how to reconcile divergent branches.");
    CHECK(git::message_of("\n\n").empty());
}

namespace {

const rummage::views::DetailSection::Group* group_named(
    const rummage::views::DetailSection& section, std::string_view id) {
    for (const auto& group : section.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

void choose_item(const std::vector<rummage::views::MenuItem>& items, std::string_view id) {
    for (const auto& item : items) {
        if (item.id == id) {
            REQUIRE(item.enabled);
            item.action();
            return;
        }
    }
    FAIL("no menu item " << id);
}

}  // namespace

TEST_CASE("git section: header, message field, commit button and folded file groups") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "a.txt", "two\n");
    write(repo / "src" / "b.txt", "b\n");
    run_git(repo, {"add", "src/b.txt"});
    write(repo / "new.txt", "n\n");
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    REQUIRE(shown.wait_for([&] { return shown.text().starts_with("main"); }));
    const rummage::views::DetailSection* section = shown.decorator.details(*shown.folder);
    REQUIRE(section != nullptr);

    REQUIRE(section->header.has_value());
    CHECK(section->header->title == "repo");
    CHECK(section->header->subtitle == "main*");
    REQUIRE(section->header->buttons.size() == 2);
    CHECK_FALSE(section->header->buttons[0].enabled);  // Sync: no upstream
    const auto more = section->header->buttons[1].menu();
    CHECK(std::ranges::any_of(more, [](const auto& item) { return item.id == "git_pull"; }));
    CHECK_FALSE(
        std::ranges::any_of(more, [](const auto& item) { return item.id == "git_commit"; }));

    REQUIRE(section->input.has_value());
    CHECK(section->input->placeholder == "Message (Ctrl+Enter to commit)");
    REQUIRE(section->split.has_value());
    CHECK(section->split->label == "✓ Commit");
    const auto ways = section->split->menu("x");
    CHECK(ways.size() == 4);

    // The groups and their counts; entries made on asking, with the staged or unstaged letter.
    const auto* staged = group_named(*section, "git_staged");
    const auto* changes = group_named(*section, "git_changes");
    REQUIRE(staged != nullptr);
    REQUIRE(changes != nullptr);
    CHECK(staged->count == 1);
    CHECK(changes->count == 2);
    CHECK(group_named(*section, "git_conflicts") == nullptr);
    const auto staged_entries = staged->entries();
    REQUIRE(staged_entries.size() == 1);
    CHECK(staged_entries[0].name == "b.txt");
    CHECK(staged_entries[0].detail == "src");
    CHECK(staged_entries[0].badge.text == "A");
    const auto changed_entries = changes->entries();
    REQUIRE(changed_entries.size() == 2);
    // A click shows its changes; Show in Browser is in its menu.
    staged_entries[0].open();
    REQUIRE_FALSE(host.shown.empty());
    CHECK(host.shown.back().title == "src/b.txt  ·  Staged Changes");
    choose_item(staged_entries[0].menu(), "git_reveal");
    REQUIRE(host.revealed.size() == 1);
    CHECK(host.revealed[0].first == repo);
    CHECK(host.revealed[0].second == repo / "src" / "b.txt");
    choose_item(staged_entries[0].menu(), "git_open");
    CHECK(host.opened == std::vector{repo / "src" / "b.txt"});

    // Ctrl+Enter in the field: commits what is staged, then the field empties.
    const std::uint64_t revision = section->input->revision;
    section->input->submit("Add b");
    REQUIRE(shown.wait_for([&] {
        const auto* now = shown.decorator.details(*shown.folder);
        return !shown.decorator.acting() && now != nullptr && now->input &&
               now->input->revision != revision;
    }));
    CHECK(run_git(repo, {"log", "-1", "--format=%s"}) == "Add b\n");
    section = shown.decorator.details(*shown.folder);
    CHECK(section->input->text.empty());

    // Amend: with no text, the message stays; with text, it changes.
    run_git(repo, {"add", "a.txt"});
    REQUIRE(shown.wait_for([&] { return shown.text().find("1 staged") != std::string::npos; }));
    choose_item(shown.decorator.details(*shown.folder)->split->menu(""), "git_amend");
    REQUIRE(shown.wait_for([&] {
        return !shown.decorator.acting() && shown.text().find("staged") == std::string::npos;
    }));
    CHECK(run_git(repo, {"log", "-1", "--format=%s"}) == "Add b\n");
    CHECK(run_git(repo, {"show", "--stat", "--format=", "HEAD"}).find("a.txt") !=
          std::string::npos);
    choose_item(shown.decorator.details(*shown.folder)->split->menu("Add b and a"), "git_amend");
    REQUIRE(shown.wait_for([&] {
        return !shown.decorator.acting() &&
               run_git(repo, {"log", "-1", "--format=%s"}) == "Add b and a\n";
    }));

    // The setting changes what the button says (and does).
    shown.decorator.set_options({.ignored = git::Options::Ignored::dim,
                                 .folder_status = true,
                                 .language = {},
                                 .commit_action = git::Options::CommitAction::commit_push});
    CHECK(shown.decorator.details(*shown.folder)->split->label == "✓ Commit & Push");
}

TEST_CASE("git section: commit & push, commit & sync and sync with a local remote") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path remote = dir.path / "remote.git";
    std::filesystem::create_directories(remote);
    run_git(remote, {"init", "-q", "--bare"});
    const std::filesystem::path repo = make_repository(dir);
    run_git(repo, {"remote", "add", "origin", remote.string()});
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    REQUIRE(shown.wait_for([&] { return shown.text().starts_with("main"); }));
    const auto section = [&] { return shown.decorator.details(*shown.folder); };
    const auto idle_and = [&](const std::function<bool()>& done) {
        return shown.wait_for([&] { return !shown.decorator.acting() && done(); });
    };

    // Commit & Push publishes the branch (no upstream yet).
    write(repo / "one.txt", "1\n");
    run_git(repo, {"add", "one.txt"});
    REQUIRE(shown.wait_for([&] { return shown.text().find("1 staged") != std::string::npos; }));
    choose_item(section()->split->menu("One"), "git_commit_push");
    REQUIRE(
        idle_and([&] { return run_git(remote, {"log", "-1", "--format=%s", "main"}) == "One\n"; }));

    // Someone else pushes; Commit & Sync pulls it, then pushes ours.
    run_git(dir.path, {"clone", "-q", remote.string(), "other"});
    write(dir.path / "other" / "theirs.txt", "t\n");
    run_git(dir.path / "other", {"add", "theirs.txt"});
    run_git(dir.path / "other", {"commit", "-q", "-m", "Theirs"});
    run_git(dir.path / "other", {"push", "-q"});
    write(repo / "two.txt", "2\n");
    run_git(repo, {"add", "two.txt"});
    REQUIRE(shown.wait_for([&] { return shown.text().find("1 staged") != std::string::npos; }));
    choose_item(section()->split->menu("Two"), "git_commit_sync");
    REQUIRE(idle_and([&] {
        return std::filesystem::exists(repo / "theirs.txt") &&
               run_git(remote, {"log", "--format=%s", "main"}).find("Two\n") != std::string::npos;
    }));

    // Sync alone: pull, then push.
    run_git(dir.path / "other", {"pull", "-q", "--no-rebase"});
    write(dir.path / "other" / "three.txt", "3\n");
    run_git(dir.path / "other", {"add", "three.txt"});
    run_git(dir.path / "other", {"commit", "-q", "-m", "Three"});
    run_git(dir.path / "other", {"push", "-q"});
    REQUIRE(shown.wait_for([&] { return section()->header->buttons[0].enabled; }));
    section()->header->buttons[0].action();
    REQUIRE(idle_and([&] { return std::filesystem::exists(repo / "three.txt"); }));
    CHECK(host.messages.back() == std::pair<std::string, bool>{"Synced", false});
}

TEST_CASE("git: the branches for-each-ref lists") {
    const git::Branches branches = git::parse_branches(
        "refs/heads/main\nrefs/heads/feature/x\nrefs/remotes/origin/HEAD\nrefs/remotes/origin/"
        "main\n"
        "refs/remotes/origin/topic\nrefs/tags/v1\n");
    CHECK(branches.local == std::vector<std::string>{"feature/x", "main"});
    CHECK(branches.remote == std::vector<std::string>{"origin/main", "origin/topic"});
    CHECK(git::parse_branches("") == git::Branches{});
}

TEST_CASE("git section: switching the branch from the header") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path origin = make_repository(dir, "origin");
    run_git(origin, {"branch", "topic"});
    run_git(dir.path, {"clone", "-q", origin.string(), "clone"});
    const std::filesystem::path repo = dir.path / "clone";
    run_git(repo, {"branch", "local-only"});
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    const auto branch_menu = [&] {
        return shown.decorator.details(*shown.folder)->header->subtitle_menu();
    };
    const auto has = [&](std::string_view id) {
        const auto items = branch_menu();
        return std::ranges::any_of(items, [&](const auto& item) { return item.id == id; });
    };
    REQUIRE(shown.wait_for([&] {
        const auto* section = shown.decorator.details(*shown.folder);
        return section != nullptr && section->header && has("git_branch:local-only");
    }));
    const auto items = branch_menu();
    const auto main =
        std::ranges::find(items, std::string{"git_branch:main"}, &rummage::views::MenuItem::id);
    REQUIRE(main != items.end());
    CHECK(main->checked);
    CHECK_FALSE(main->enabled);                  // already there
    CHECK(has("git_branch:origin/topic"));       // remote, no local copy
    CHECK_FALSE(has("git_branch:origin/main"));  // main exists locally

    choose_item(branch_menu(), "git_branch:local-only");
    REQUIRE(shown.wait_for(
        [&] { return !shown.decorator.acting() && shown.text().starts_with("local-only"); }));
    CHECK(host.messages.back() == std::pair<std::string, bool>{"Switched to local-only", false});

    choose_item(branch_menu(), "git_branch:origin/topic");
    REQUIRE(shown.wait_for(
        [&] { return !shown.decorator.acting() && shown.text().starts_with("topic"); }));
    CHECK(run_git(repo, {"rev-parse", "--abbrev-ref", "topic@{upstream}"}) == "origin/topic\n");

    host.message_to_type = "bad name";
    choose_item(branch_menu(), "git_new_branch");
    CHECK(host.refused == "Not a valid branch name");
    host.message_to_type = "fresh";
    choose_item(branch_menu(), "git_new_branch");
    CHECK(host.refused.empty());
    CHECK(host.actions.back() == "Create");
    REQUIRE(shown.wait_for(
        [&] { return !shown.decorator.acting() && shown.text().starts_with("fresh"); }));
}

TEST_CASE("git: a diff as a text page") {
    using rummage::text::Style;
    const auto text = git::diff_text(
        "diff --git a/a.txt b/a.txt\nindex 1..2 100644\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n"
        "-old\n+new\tx\n context\n\xff broken\n");
    REQUIRE(text.lines.size() == 9);
    CHECK(text.language == "Diff");
    CHECK(text.lines[6] == "+new    x");  // the tab written out
    CHECK(text.lines[8] == "\xEF\xBF\xBD broken");
    const auto style = [&](std::size_t line) {
        return text.spans[line].empty() ? std::optional<Style>{} : text.spans[line].front().style;
    };
    CHECK(style(0) == Style::comment);
    CHECK(style(3) == Style::comment);  // +++ is a header, not an added line
    CHECK(style(4) == Style::function);
    CHECK(style(5) == Style::removed);
    CHECK(style(6) == Style::added);
    CHECK_FALSE(style(7).has_value());
    CHECK(text.spans[6].front().length == text.lines[6].size());

    const auto binary = git::diff_text("Binary files a/x.png and b/x.png differ\n");
    CHECK(binary.spans.front().front().style == Style::comment);
    std::string many;
    for (int i = 0; i < 30; ++i) {
        many += "+line\n";
    }
    const auto cut = git::diff_text(many, 10);
    CHECK(cut.lines.size() == 10);
    CHECK(cut.truncated);
    CHECK(git::diff_text("").lines.empty());
}

TEST_CASE("git section: the changes of a file, and the arrows through the list") {
    const GitIdentity identity;
    const TempDir dir;
    const std::filesystem::path repo = make_repository(dir);
    write(repo / "a.txt", "two\n");
    write(repo / "new.txt", "brand new\n");
    Shown shown{repo};
    TestHost host;
    shown.decorator.set_host(host.host());
    REQUIRE(shown.wait_for([&] { return shown.text().find("1 changed") != std::string::npos; }));
    const auto last_text = [&] {
        return host.shown.empty() ? std::string{} : [&] {
            std::string all;
            for (const auto& line : host.shown.back().text.lines) {
                all += line + "\n";
            }
            return all;
        }();
    };

    // From the browser's Git menu.
    REQUIRE(choose(shown, {"a.txt"}, "git_diff"));
    CHECK(host.shown.back().text.lines == std::vector<std::string>{"Loading…"});
    REQUIRE(shown.wait_for([&] { return last_text().find("+two\n") != std::string::npos; }));
    CHECK(last_text().find("-one\n") != std::string::npos);
    CHECK(host.shown.back().title == "a.txt  ·  Changes");

    // From the section's list: an untracked file's whole text is added; the arrows step.
    const auto* changes = group_named(*shown.decorator.details(*shown.folder), "git_changes");
    REQUIRE(changes != nullptr);
    const auto entries = changes->entries();
    REQUIRE(entries.size() == 2);
    entries[1].open();  // new.txt
    REQUIRE(shown.wait_for([&] { return last_text().find("+brand new\n") != std::string::npos; }));
    CHECK(host.shown.back().title == "new.txt  ·  Untracked");
    host.shown.back().step(-1);
    REQUIRE(shown.wait_for([&] { return last_text().find("+two\n") != std::string::npos; }));
    const std::size_t before = host.shown.size();
    host.shown.back().step(-1);  // nothing before the first
    run_for(shown.loop, 100ms);
    CHECK(host.shown.size() == before);
}
