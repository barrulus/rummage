#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/text/text.hpp"

// A file's changes as git shows them (`git diff`), ready for a text page: one column, removed
// lines red, added ones green.
namespace rummage::git {

// Which changes of a path.
enum class DiffKind : std::uint8_t {
    changes,    // the working tree against the index (what is not staged)
    staged,     // the index against HEAD
    untracked,  // a new file, all of it added
};

// The arguments after the program for path's changes (relative to worktree).
[[nodiscard]] std::vector<std::string> diff_arguments(const std::filesystem::path& worktree,
                                                      const std::string& path, DiffKind kind);

// What `git diff` printed as a text page: its lines (tabs written out, broken UTF-8 replaced,
// long lines cut), coloured: git's headers as comments, hunk headers as functions, + added,
// - removed. At most max_lines lines; truncated when there were more.
[[nodiscard]] text::Text diff_text(std::string_view output, std::size_t max_lines = 20000);

}  // namespace rummage::git
