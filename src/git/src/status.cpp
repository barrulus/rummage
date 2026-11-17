#include "rummage/git/status.hpp"

#include <array>
#include <charconv>

namespace rummage::git {
namespace {

// The next NUL-terminated record of output, advancing past it; nullopt at the end. A last
// record without its NUL still counts.
std::optional<std::string_view> next_record(std::string_view& output) {
    if (output.empty()) {
        return std::nullopt;
    }
    const std::size_t end = output.find('\0');
    const std::string_view record = output.substr(0, end);
    output.remove_prefix(end == std::string_view::npos ? output.size() : end + 1);
    return record;
}

// Skips count space-separated fields of record; false when it has fewer.
bool skip_fields(std::string_view& record, int count) {
    for (int i = 0; i < count; ++i) {
        const std::size_t space = record.find(' ');
        if (space == std::string_view::npos) {
            return false;
        }
        record.remove_prefix(space + 1);
    }
    return true;
}

bool parse_count(std::string_view text, std::int64_t& out) {
    const char* const end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_header(std::string_view header, Branch& branch) {
    const std::size_t space = header.find(' ');
    const std::string_view key = header.substr(0, space);
    const std::string_view value =
        space == std::string_view::npos ? std::string_view{} : header.substr(space + 1);
    if (key == "branch.oid") {
        branch.oid = value == "(initial)" ? std::string{} : std::string{value};
    } else if (key == "branch.head") {
        branch.name = value == "(detached)" ? std::string{} : std::string{value};
    } else if (key == "branch.upstream") {
        branch.upstream = value;
    } else if (key == "branch.ab") {
        const std::size_t gap = value.find(' ');
        if (gap == std::string_view::npos || !value.starts_with('+') ||
            value.substr(gap + 1).size() < 2 || value[gap + 1] != '-' ||
            !parse_count(value.substr(1, gap - 1), branch.ahead) ||
            !parse_count(value.substr(gap + 2), branch.behind)) {
            return false;
        }
    }
    return true;  // headers rummage does not know (stash, ...) are fine
}

}  // namespace

Status::Counts Status::counts() const {
    Counts counts;
    for (const FileStatus& file : files) {
        switch (file.kind) {
            case FileStatus::Kind::tracked:
                counts.staged += file.staged != '.' ? 1U : 0U;
                counts.changed += file.unstaged != '.' ? 1U : 0U;
                break;
            case FileStatus::Kind::untracked:
                ++counts.untracked;
                break;
            case FileStatus::Kind::conflict:
                ++counts.conflicts;
                break;
            case FileStatus::Kind::ignored:
                break;
        }
    }
    return counts;
}

std::vector<std::string> status_arguments(const std::filesystem::path& worktree, bool ignored) {
    std::vector<std::string> arguments{
        "--no-optional-locks", "-C", worktree.string(), "status",
        "--porcelain=v2",      "-z", "--branch",        "--untracked-files=all"};
    if (ignored) {
        arguments.emplace_back("--ignored=matching");
    }
    return arguments;
}

std::optional<Status> parse_status(std::string_view output) {
    Status status;
    while (const std::optional<std::string_view> next = next_record(output)) {
        std::string_view record = *next;
        if (record.size() < 2 || record[1] != ' ') {
            return std::nullopt;
        }
        const char type = record.front();
        if (type == '#') {
            if (!parse_header(record.substr(2), status.branch)) {
                return std::nullopt;
            }
            continue;
        }
        FileStatus file;
        if (type == '?' || type == '!') {
            file.kind = type == '?' ? FileStatus::Kind::untracked : FileStatus::Kind::ignored;
            file.path = record.substr(2);
        } else if (type == '1' || type == '2' || type == 'u') {
            record.remove_prefix(2);
            if (record.size() < 3 || record[2] != ' ') {
                return std::nullopt;
            }
            file.staged = record[0];
            file.unstaged = record[1];
            file.kind = type == 'u' ? FileStatus::Kind::conflict : FileStatus::Kind::tracked;
            // 1: XY sub mH mI mW hH hI path; 2: one more (Xscore); u: XY sub m1 m2 m3 mW h1 h2 h3.
            const int fields = type == '1' ? 7 : type == '2' ? 8 : 9;
            if (!skip_fields(record, fields)) {
                return std::nullopt;
            }
            file.path = record;
            if (type == '2') {
                const std::optional<std::string_view> original = next_record(output);
                if (!original) {
                    return std::nullopt;
                }
                file.original = *original;
            }
        } else {
            return std::nullopt;
        }
        if (file.path.empty()) {
            return std::nullopt;
        }
        status.files.push_back(std::move(file));
    }
    return status;
}

std::vector<std::string> commit_arguments(const std::filesystem::path& worktree) {
    return {"--no-optional-locks",
            "-C",
            worktree.string(),
            "log",
            "-1",
            "--no-show-signature",
            "--no-color",
            "--format=%h%x00%s%x00%an%x00%ct"};
}

std::optional<Commit> parse_commit(std::string_view output) {
    while (output.ends_with('\n')) {
        output.remove_suffix(1);
    }
    Commit commit;
    const std::array<std::string*, 3> fields{&commit.hash, &commit.subject, &commit.author};
    for (std::string* const field : fields) {
        const std::size_t end = output.find('\0');
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        *field = output.substr(0, end);
        output.remove_prefix(end + 1);
    }
    if (commit.hash.empty() || !parse_count(output, commit.time)) {
        return std::nullopt;
    }
    return commit;
}

}  // namespace rummage::git
