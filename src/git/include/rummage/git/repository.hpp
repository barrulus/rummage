#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

// The git repository a folder belongs to, found without running git: rummage looks for ".git"
// upwards as git does, so a folder outside any repository costs a few stat calls and no process.
namespace rummage::git {

struct Repository {
    std::filesystem::path worktree;  // the top folder of the working tree
    // Its git directory: worktree/.git, or where a ".git" file points (linked worktrees,
    // submodules). HEAD, the index and the files of a merge or rebase in progress are there.
    std::filesystem::path git_dir;
    // Where refs/ and packed-refs are: git_dir, or the main repository's for a linked worktree.
    std::filesystem::path common_dir;

    bool operator==(const Repository&) const = default;
};

// The repository folder is in, itself or one of its parents: the first ".git" upwards that is a
// directory with a HEAD, or a file "gitdir: <path>". The search stops where the file system
// changes, as git's does. nullopt outside a repository, in a bare one and inside a git directory.
// Blocking (stats each parent): call it from a worker.
[[nodiscard]] std::optional<Repository> find_repository(const std::filesystem::path& folder);

// What git is in the middle of, from the files it leaves in the git directory.
enum class Operation : std::uint8_t { none, merge, rebase, cherry_pick, revert, bisect };

// Blocking: stats a few files.
[[nodiscard]] Operation operation_of(const Repository& repository);

}  // namespace rummage::git
