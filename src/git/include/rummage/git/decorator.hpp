#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/git/diff.hpp"
#include "rummage/git/repository.hpp"
#include "rummage/git/status.hpp"
#include "rummage/views/decorations.hpp"

namespace rummage::platform {
class Launcher;
class ProgramOutput;
}  // namespace rummage::platform

// The git integration as the views see it: which repository a folder on screen is in, its branch
// and changes in the status bar, a badge on each changed entry. Read-only: rummage never runs a
// git command that writes.
namespace rummage::git {

struct Options {
    enum class Ignored : std::uint8_t { show, dim, hide };

    Ignored ignored{Ignored::dim};
    bool folder_status{true};  // folders show the changes inside them
    // What git's texts and git's own messages are in: a language code ("en"), or empty for the
    // interface's. Its texts follow i18n's "git" context, which the owner sets alike.
    std::string language;
    // What the commit button and Ctrl+Enter in the message field do; the button's menu has all.
    enum class CommitAction : std::uint8_t { commit, commit_push, commit_sync };
    CommitAction commit_action{CommitAction::commit};
    // How states are coloured: as editors do (by kind of change), or as git does (staged green,
    // the rest red).
    enum class Colors : std::uint8_t { palette, editor, git };
    Colors colors{Colors::palette};

    bool operator==(const Options&) const = default;
};

// badge in a colour scheme: as is for Options::Colors::palette and editor (the views colour
// those through theme::status_scheme); git's for Options::Colors::git.
[[nodiscard]] views::Badge in_scheme(views::Badge badge, const FileStatus& file,
                                     Options::Colors colors);

// The badge of one path git reports; nullopt for one without (an unchanged path).
[[nodiscard]] std::optional<views::Badge> badge_of(const FileStatus& file);

// What a badge stands for, in the interface's language ("Modified, staged").
[[nodiscard]] std::string meaning_of(const FileStatus& file);

// The status bar's text: branch (or the commit of a detached HEAD), ahead/behind, the operation
// in progress and the counts ("main ↑1 ↓2  ·  Merging  ·  1 conflict  ·  3 changed").
[[nodiscard]] std::string summary_of(const Status& status, Operation operation,
                                     std::string_view short_hash);

// "Just now", "5 minutes ago", "3 days ago"; then: seconds since the epoch.
[[nodiscard]] std::string time_ago(std::int64_t then, std::int64_t now);

// A remote as the information pane shows it: its address without any user, password or token,
// and a web page to open only for https remotes and the big hosts (GitHub, GitLab, Bitbucket,
// Codeberg, SourceHut), whose ssh addresses map to their pages.
struct RemoteView {
    std::string shown;                // "github.com/a/b", "ssh://host:2222/a/b.git", a path
    std::optional<std::string> link;  // "https://github.com/a/b"

    bool operator==(const RemoteView&) const = default;
};

[[nodiscard]] RemoteView view_of_remote(std::string_view remote);

// The branches of `git for-each-ref --format=%(refname) refs/heads refs/remotes`: local names,
// and remote ones ("origin/x", without the remotes' HEAD), each sorted.
struct Branches {
    std::vector<std::string> local;
    std::vector<std::string> remote;

    bool operator==(const Branches&) const = default;
};

[[nodiscard]] Branches parse_branches(std::string_view output);

// What git said went wrong, for a message: its first line that is not a hint, without git's
// prefix ("fatal: ", "error: ", or theirs in git's language: "Schwerwiegend: ").
[[nodiscard]] std::string message_of(const std::string& output);

// "a1b2c3d Fix the thing  ·  Ada  ·  3 days ago".
[[nodiscard]] std::string commit_line(const Commit& commit, std::int64_t now);

// Loop thread only. Must not outlive the loop, the pool or the watcher.
class Decorator final : public views::Decorations {
public:
    static constexpr std::chrono::milliseconds kSettle{300};  // after the last change
    static constexpr std::chrono::milliseconds kTimeLimit{5000};
    static constexpr std::size_t kRepositories = 6;  // followed at once; the oldest goes

    // git: the program; redraw: something shown changed.
    Decorator(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
              std::filesystem::path git, std::function<void()> redraw, Options options = {});
    ~Decorator() override;

    Decorator(const Decorator&) = delete;
    Decorator& operator=(const Decorator&) = delete;
    Decorator(Decorator&&) = delete;
    Decorator& operator=(Decorator&&) = delete;

    void set_options(const Options& options);

    [[nodiscard]] const Options& options() const noexcept { return options_; }

    // Asks the desktop for an activation token before a link opens, so the browser comes to the
    // front (the window's; done("") without one).
    void set_activation(std::function<void(std::function<void(std::string)> done)> request) {
        activation_ = std::move(request);
    }

    // Runs when what hides() answers changes: the listings must be filtered again.
    void set_refilter_handler(std::function<void()> refilter) { refilter_ = std::move(refilter); }

    [[nodiscard]] const views::Badge* badge(const views::Folder& folder,
                                            const fs::Entry& entry) override;
    [[nodiscard]] const views::StatusPart* status(const views::Folder& folder) override;
    [[nodiscard]] const views::DetailSection* details(const views::Folder& folder) override;

    void set_host(views::DecorationHost host) override { host_ = std::move(host); }

    // Git's commands for names in folder (stage, commit, ignore...), or for the whole repository
    // on the folder's background (commit, fetch, pull, push...).
    [[nodiscard]] std::optional<views::MenuItem> menu(
        const views::Folder& folder, const std::vector<std::string>& names) override;

    // A command that changes a repository is running (tests).
    [[nodiscard]] bool acting() const;

    // How long a fetch, pull or push may take before it is stopped.
    static constexpr std::chrono::milliseconds kNetworkLimit{120000};

    // Ignored entries, with Ignored::hide.
    [[nodiscard]] bool hides_any() const override {
        return options_.ignored == Options::Ignored::hide;
    }

    [[nodiscard]] bool hides(const std::filesystem::path& folder, const fs::Entry& entry) override;
    // Conflicts first, then changes, new files, unchanged and ignored ones.
    [[nodiscard]] int rank(const std::filesystem::path& folder, const fs::Entry& entry) override;

    // How long git status may take before the repository is paused (tests shorten it).
    void set_time_limit(std::chrono::milliseconds limit) { time_limit_ = limit; }

    // git queries started so far (tests: debouncing, nothing outside repositories).
    [[nodiscard]] std::size_t queries() const noexcept { return queries_; }

private:
    struct Repo;
    struct Place;  // a folder: its repository and where it is inside it

    Place* place_of(const views::Folder& folder);
    void detect(const std::filesystem::path& folder);
    Repo& follow(const Repository& repository);
    void forget_oldest();
    void schedule(Repo& repo);
    void run(Repo& repo);
    void finished(Repo& repo, const std::string& output, int wait_status, Operation operation);
    void read_commit(Repo& repo);
    void read_remote(Repo& repo);
    void read_branches(Repo& repo);
    [[nodiscard]] std::vector<views::MenuItem> branch_items(Repo& repo);
    void open_link(const std::string& url);
    void rebuild(Repo& repo);
    using Steps = std::vector<std::vector<std::string>>;
    void act(Repo& repo, std::string name, Steps steps, std::string done,
             std::function<void()> after = {});
    struct Acting;
    void step(Repo& repo, std::shared_ptr<Acting> acting);
    void ask_commit(Repo& repo, Steps before, std::vector<std::string> paths);
    void commit_all(Repo& repo);
    void discard(Repo& repo, const std::vector<std::string>& paths);
    void ignore(Repo& repo, std::vector<std::string> paths);
    [[nodiscard]] std::vector<views::DetailSection::Button> buttons(Repo& repo);
    [[nodiscard]] std::vector<views::MenuItem> repo_items(Repo& repo, bool with_commit);
    [[nodiscard]] std::vector<views::MenuItem> path_items(Repo& repo,
                                                          const std::vector<std::string>& paths,
                                                          bool inside_ignored);
    [[nodiscard]] static std::vector<std::string> push_arguments(const Repo& repo);
    [[nodiscard]] static std::vector<std::string> pull_arguments(const Repo& repo);
    enum class CommitMode : std::uint8_t { plain, amend, push, sync };
    void commit_typed(Repo& repo, const std::string& typed, CommitMode mode);
    void sync(Repo& repo);

    // A file's changes in Quick Look; the arrows go through items from index.
    struct DiffItem {
        std::string path;
        DiffKind kind{DiffKind::changes};
    };

    void show_diff(const std::string& key, std::vector<DiffItem> items, std::size_t index);
    // which: 0 conflicts, 1 staged, 2 changes (the section's groups).
    void show_group_diff(Repo& repo, int which, const std::string& path);
    [[nodiscard]] std::vector<views::DetailSection::Button> entry_actions(
        Repo& repo, int which, const std::string& path, const std::filesystem::path& target);
    [[nodiscard]] static std::vector<DiffItem> diff_items(const Status& status,
                                                          const std::vector<std::string>& paths);
    [[nodiscard]] const views::Badge* badge_at(Place& place, const std::string& name);
    [[nodiscard]] const views::Badge* lookup(const Repo& repo, const Place& place,
                                             const std::string& name) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::filesystem::path git_;
    std::function<void()> redraw_;
    std::function<void()> refilter_;
    views::DecorationHost host_;
    std::function<void(std::function<void(std::string)>)> activation_;
    std::unique_ptr<platform::ProgramOutput> diff_query_;  // the diff being read for Quick Look
    std::uint64_t diff_generation_{0};
    std::unique_ptr<platform::Launcher> launcher_;  // xdg-open, detached, its output dropped
    Options options_;
    std::chrono::milliseconds time_limit_{kTimeLimit};
    std::unordered_map<std::string, std::unique_ptr<Repo>> repos_;    // by working tree
    std::unordered_map<std::string, std::unique_ptr<Place>> places_;  // by folder
    // The last folder asked about, so the rows of one draw find it without a lookup.
    const views::Folder* last_folder_{nullptr};
    std::uint64_t last_revision_{0};
    Place* last_place_{nullptr};
    std::uint64_t asked_{0};  // a clock of questions, for forget_oldest()
    std::size_t queries_{0};
    views::StatusPart part_;
    // What part_ was made from: built again only when one of these changes.
    const Repo* part_repo_{nullptr};
    std::uint64_t part_generation_{0};
    std::string part_cursor_;
    std::int64_t part_minute_{0};
    views::DetailSection section_;
    const Repo* section_repo_{nullptr};
    std::uint64_t section_generation_{0};
    std::int64_t section_minute_{0};
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::git
