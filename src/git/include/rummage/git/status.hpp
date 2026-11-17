#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A working tree's state as git reports it to programs: `git status --porcelain=v2 -z --branch`
// (https://git-scm.com/docs/git-status#_porcelain_format_version_2) and the last commit from
// `git log -1`. Parsing only: running git is the caller's business (status_arguments()).
namespace rummage::git {

struct Branch {
    std::string name;      // empty: HEAD is detached
    std::string oid;       // the commit HEAD names; empty before the first commit
    std::string upstream;  // empty: none set
    std::int64_t ahead{0};
    std::int64_t behind{0};

    bool operator==(const Branch&) const = default;
};

// One path git reports. staged and unstaged are porcelain's X and Y: '.' (unchanged), 'M', 'T',
// 'A', 'D', 'R', 'C', and for a conflict 'U' or the side that added or deleted it.
struct FileStatus {
    enum class Kind : std::uint8_t { tracked, untracked, ignored, conflict };

    std::string path;      // relative to the working tree, '/'-separated
    std::string original;  // a rename's or copy's source; empty otherwise
    Kind kind{Kind::tracked};
    char staged{'.'};
    char unstaged{'.'};

    bool operator==(const FileStatus&) const = default;
};

struct Status {
    Branch branch;
    std::vector<FileStatus> files;  // in git's order (sorted by path)

    // Paths with a change in the index; with one in the working tree; untracked; in conflict.
    struct Counts {
        std::size_t staged{0};
        std::size_t changed{0};
        std::size_t untracked{0};
        std::size_t conflicts{0};

        bool operator==(const Counts&) const = default;
    };

    [[nodiscard]] Counts counts() const;
};

// The arguments after the program for the status of worktree. ignored also lists ignored
// paths (whole ignored folders as one, as git's "matching" mode does). No optional locks: git
// leaves the index alone while another git writes it.
[[nodiscard]] std::vector<std::string> status_arguments(const std::filesystem::path& worktree,
                                                        bool ignored);

// What `git status` with those arguments printed; nullopt when it is not porcelain v2.
[[nodiscard]] std::optional<Status> parse_status(std::string_view output);

struct Commit {
    std::string hash;  // abbreviated
    std::string subject;
    std::string author;
    std::int64_t time{0};  // seconds since the epoch, committer date

    bool operator==(const Commit&) const = default;
};

// The arguments for the commit HEAD names.
[[nodiscard]] std::vector<std::string> commit_arguments(const std::filesystem::path& worktree);

// What `git log` with commit_arguments() printed; nullopt for anything else (no commit yet).
[[nodiscard]] std::optional<Commit> parse_commit(std::string_view output);

}  // namespace rummage::git
