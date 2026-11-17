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
#include "rummage/git/repository.hpp"
#include "rummage/git/status.hpp"
#include "rummage/views/decorations.hpp"

namespace rummage::platform {
class ProgramOutput;
}

// The git integration as the views see it: which repository a folder on screen is in, its branch
// and changes in the status bar, a badge on each changed entry. Read-only: rummage never runs a
// git command that writes.
namespace rummage::git {

struct Options {
    enum class Ignored : std::uint8_t { show, dim, hide };

    Ignored ignored{Ignored::dim};
    bool folder_status{true};  // folders show the changes inside them

    bool operator==(const Options&) const = default;
};

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

    // Whether entry is left out (an ignored one with Ignored::hide). The browser filters with it.
    [[nodiscard]] bool hides(const views::Folder& folder, const fs::Entry& entry);

    [[nodiscard]] const views::Badge* badge(const views::Folder& folder,
                                            const fs::Entry& entry) override;
    [[nodiscard]] const views::StatusPart* status(const views::Folder& folder) override;

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
    void rebuild(Repo& repo);
    [[nodiscard]] const views::Badge* lookup(const Repo& repo, const Place& place,
                                             const std::string& name) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::filesystem::path git_;
    std::function<void()> redraw_;
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
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::git
