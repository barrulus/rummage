#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/git/repository.hpp"
#include "rummage/git/status.hpp"
#include "temp_dir.hpp"

namespace git = rummage::git;
using git::FileStatus;
using rummage::test::TempDir;

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
