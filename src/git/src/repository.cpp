#include "rummage/git/repository.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace rummage::git {
namespace {

bool present(const std::filesystem::path& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0;
}

// The first line of a small file, without its line end; empty when it cannot be read.
std::string first_line(const std::filesystem::path& file) {
    std::ifstream in{file};
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::filesystem::path without_trailing_slash(std::filesystem::path path) {
    path = path.lexically_normal();
    if (!path.has_filename() && path.has_parent_path() && path != path.root_path()) {
        path = path.parent_path();
    }
    return path;
}

// A path a git file names, relative to the folder the file is in unless absolute.
std::filesystem::path resolve(const std::filesystem::path& base, std::string_view named) {
    const std::filesystem::path path{named};
    return without_trailing_slash(path.is_absolute() ? path : base / path);
}

std::filesystem::path common_dir_of(const std::filesystem::path& git_dir) {
    const std::string common = first_line(git_dir / "commondir");
    return common.empty() ? git_dir : resolve(git_dir, common);
}

}  // namespace

std::optional<Repository> find_repository(const std::filesystem::path& folder) {
    std::filesystem::path dir = without_trailing_slash(folder);
    for (const std::filesystem::path& part : dir) {
        if (part == ".git") {
            return std::nullopt;
        }
    }
    struct stat here{};
    if (stat(dir.c_str(), &here) != 0) {
        return std::nullopt;
    }
    for (;;) {
        const std::filesystem::path dot_git = dir / ".git";
        struct stat info{};
        if (stat(dot_git.c_str(), &info) == 0) {
            if (S_ISDIR(info.st_mode) && present(dot_git / "HEAD")) {
                return Repository{dir, dot_git, common_dir_of(dot_git)};
            }
            if (S_ISREG(info.st_mode)) {
                constexpr std::string_view kPrefix = "gitdir: ";
                const std::string line = first_line(dot_git);
                if (line.starts_with(kPrefix)) {
                    const std::filesystem::path git_dir =
                        resolve(dir, std::string_view{line}.substr(kPrefix.size()));
                    if (present(git_dir / "HEAD")) {
                        return Repository{dir, git_dir, common_dir_of(git_dir)};
                    }
                }
            }
        }
        const std::filesystem::path parent = dir.parent_path();
        struct stat above{};
        if (parent == dir || stat(parent.c_str(), &above) != 0 || above.st_dev != here.st_dev) {
            return std::nullopt;
        }
        dir = parent;
        here = above;
    }
}

Operation operation_of(const Repository& repository) {
    const std::filesystem::path& dir = repository.git_dir;
    if (present(dir / "rebase-merge") || present(dir / "rebase-apply")) {
        return Operation::rebase;
    }
    if (present(dir / "MERGE_HEAD")) {
        return Operation::merge;
    }
    if (present(dir / "CHERRY_PICK_HEAD")) {
        return Operation::cherry_pick;
    }
    if (present(dir / "REVERT_HEAD")) {
        return Operation::revert;
    }
    if (present(dir / "BISECT_LOG")) {
        return Operation::bisect;
    }
    return Operation::none;
}

}  // namespace rummage::git
