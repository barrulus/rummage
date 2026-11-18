#pragma once

// The decorator's own state of a repository and of a folder, shared by its source files.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rummage/core/i18n.hpp"
#include "rummage/git/decorator.hpp"
#include "rummage/platform/program_output.hpp"

namespace rummage::git {

using views::Badge;

inline constexpr std::string_view kSeparator = "  ·  ";

inline std::string_view operation_name(Operation operation) {
    switch (operation) {
        case Operation::merge:
            return i18n::trc("git", "Merging");
        case Operation::rebase:
            return i18n::trc("git", "Rebasing");
        case Operation::cherry_pick:
            return i18n::trc("git", "Cherry-picking");
        case Operation::revert:
            return i18n::trc("git", "Reverting");
        case Operation::bisect:
            return i18n::trc("git", "Bisecting");
        case Operation::none:
            break;
    }
    return {};
}

inline std::int64_t seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Decorator::Repo {
    Repository repository;
    std::string key;  // the working tree, as repos_ knows it
    std::vector<fs::DirectoryWatcher::WatchId> watches;
    std::unique_ptr<platform::ProgramOutput> query;
    std::unique_ptr<platform::ProgramOutput> commit_query;
    core::EventLoop::SourceId settle{0};
    core::EventLoop::SourceId limit{0};
    std::chrono::steady_clock::time_point pending_since{};
    bool again{false};      // something changed while a query ran
    bool paused{false};     // the last query took too long
    bool failed{false};     // git would not tell (not a repository after all, unsafe owner...)
    bool reading{false};    // a query or what follows it is on its way
    std::string paused_in;  // the folder shown when it did
    std::optional<Status> status;
    Operation operation{Operation::none};
    std::optional<Commit> commit;
    // Badges by path in the working tree; folders' summaries apart from git's own entries.
    std::unordered_map<std::string, Badge> files;
    std::unordered_map<std::string, Badge> folders;
    std::unordered_map<std::string, std::size_t> index;  // path -> status->files
    std::vector<std::string> ignored_folders;            // "build/"
    std::size_t ignored_signature{0};
    std::size_t rank_signature{0};  // of every badge, to sort by status again when one changes //
                                    // of every ignored path, to see changes
    std::uint64_t generation{0};    // rebuilt
    std::uint64_t asked{0};
    // A command that changes the repository (stage, commit, push...): one at a time, its steps in
    // order, stopping at the first that fails.
    std::unique_ptr<platform::ProgramOutput> action;
    core::EventLoop::SourceId action_limit{0};
    std::string action_name;  // what runs, for the information pane ("Push")
    // The upstream's remote (or origin) as a web page, for the information pane's link.
    std::unique_ptr<platform::ProgramOutput> remote_query;
    std::string remote_name;  // what remote_url is about; empty: not read yet
    bool remote_stale{true};  // .git/config changed
    std::optional<RemoteView> remote;
    bool pull_configured{false};  // pull.rebase or pull.ff set: pull as configured, else merge
    // The information pane's message field: emptied (revision moves on) after a commit.
    std::uint64_t input_revision{0};
    std::string input_text;
};

struct Decorator::Place {
    std::string folder;
    Repo* repo{nullptr};  // nullptr: in none, or not known yet
    std::string prefix;   // the folder in the working tree: "" at its top, else "src/deep/"
    std::uint64_t revision{0};
    bool known{false};
    bool detecting{false};
    std::uint64_t ignored_generation{~std::uint64_t{0}};
    bool ignored{false};  // inside an ignored folder, as of ignored_generation
};

}  // namespace rummage::git
