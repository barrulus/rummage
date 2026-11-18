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
    CHECK(value_of("Branch") == "main");
    CHECK(value_of("Changes") == "1 changed, 2 untracked");
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
        shown.decorator.set_options(
            {.ignored = git::Options::Ignored::hide, .folder_status = false, .language = {}});
        CHECK(shown.hides("debug.log"));
        CHECK_FALSE(shown.hides("a.txt"));
        CHECK(shown.badge("sub") == nullptr);
        shown.decorator.set_options(
            {.ignored = git::Options::Ignored::show, .folder_status = true, .language = {}});
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
                {.ignored = git::Options::Ignored::hide, .folder_status = true, .language = {}}};
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

    shown.decorator.set_options(
        {.ignored = git::Options::Ignored::dim, .folder_status = true, .language = {}});
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
    REQUIRE(idle_and([&] {
        return shown.decorator.details(*shown.folder) != nullptr &&
               !shown.decorator.details(*shown.folder)->rows.empty() &&
               shown.decorator.details(*shown.folder)->rows[1].value == "origin/main  ↑0 ↓0";
    }));
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
    shown.decorator.set_options(
        {.ignored = git::Options::Ignored::dim, .folder_status = true, .language = "en"});
    CHECK(shown.text() == "main  ·  1 changed");
    CHECK(shown.decorator.menu(*shown.folder, {"a.txt"})->children.front().label == "Stage");
    CHECK(rummage::i18n::tr("Delete") != "Delete");  // the interface stays German

    rummage::i18n::set_context_language("git", "");
    shown.decorator.set_options(
        {.ignored = git::Options::Ignored::dim, .folder_status = true, .language = {}});
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
    CHECK((row != nullptr && row->action != nullptr));
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
    CHECK(git::message_of("\n\n").empty());
}
