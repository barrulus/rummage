#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/applications.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/launcher.hpp"

namespace rummage::platform {

// A file to open, by absolute path, and its type.
struct OpenFile {
    std::filesystem::path path;
    std::string mime;
};

// What opening some files does: the processes to start, and what cannot be opened.
struct OpenPlan {
    struct Launch {
        std::string app_id;
        std::string app_name;  // the localized Name, else the ID
        std::vector<std::string> argv;
        std::filesystem::path directory;    // the entry's Path, else the first file's folder
        std::size_t files{0};               // how many of the files it opens
        std::shared_ptr<int> executable{};  // pinned checked/approved inode, inherited as fd 3
        std::vector<std::filesystem::path> recent_paths{};
        bool program{false};  // the file itself, started as a program
    };

    std::vector<Launch> launches;
    // Ready to show: "No application opens image/x-foo", "No terminal for Helix".
    std::vector<std::string> problems;
    std::size_t files{0};    // the files with an application
    std::string first_name;  // the first of them, by name
    // Programs that are not allowed to run (no execute bit): Rummage asks before allowing one.
    std::vector<std::filesystem::path> not_allowed;
};

// The types a program Rummage may start has (as found by name, then content): an executable,
// an AppImage, or what shared-mime-info calls a position-independent executable (a shared
// library). A program named like a document ("invoice.pdf") is a document.
[[nodiscard]] bool may_be_program(const fs::MimeDatabase& database, fs::MimeId mime);

// Groups files by their default application (in the order they come) and expands each group's
// Exec line: files for one application go to one process when it takes a list. Terminal=true
// applications run inside in_terminal(). With app_id, every file goes to that application
// ("Open With").
[[nodiscard]] OpenPlan plan_open(std::span<const OpenFile> files, const Applications& apps,
                                 const fs::MimeDatabase& database, const Environment& environment,
                                 std::string_view app_id = {});

// An application to offer for a file ("Open With").
struct AppChoice {
    std::string id;
    std::string name;  // the localized Name, else the ID
    bool is_default{false};
};

// The applications for a type: the default first, then Applications::all_for's.
[[nodiscard]] std::vector<AppChoice> choices_for(std::string_view mime, const Applications& apps,
                                                 const fs::MimeDatabase& database);

// Opens files with their default applications (DESIGN, "Apertura dei file"): types and
// applications are worked out on a worker, the processes start on the loop thread, each with an
// activation token when one can be had, and what happened is reported in one line.
//
// Loop thread only. Must not outlive the loop or the pool.
class Opener {
public:
    // A fresh xdg-activation token for the next process, handed to done (at once with "" when
    // there is none to be had).
    using TokenRequest = std::function<void(std::function<void(std::string token)> done)>;
    // A line for the user: "Opening photo.orf with darktable…", or an error.
    using Report = std::function<void(std::string message, bool error)>;
    // A program without its execute bit was opened: the user is asked, and allow_and_run()
    // gives it u+x and starts it.
    using AskToRun =
        std::function<void(std::filesystem::path program, std::function<void()> allow_and_run)>;

    Opener(core::EventLoop& loop, core::WorkerPool& pool,
           std::shared_ptr<const fs::MimeDatabase> database, Environment environment, Report report,
           TokenRequest token = nullptr);
    ~Opener();

    Opener(const Opener&) = delete;
    Opener& operator=(const Opener&) = delete;
    Opener(Opener&&) = delete;
    Opener& operator=(Opener&&) = delete;

    // Opens directory's entries (not folders: callers go into those). Entries with a known type
    // keep it; the others are detected as the views would.
    // With app_id, all of them with that application, folders too ("Open With" on a folder).
    // Without, a program (an ELF executable or AppImage, by type and content) starts by itself
    // in its folder when it may run and programs() allows it; one that may not run is asked
    // about (set_ask_to_run), never started otherwise; one on a noexec disk is reported.
    // Scripts are never started: they open as text.
    void open(const std::filesystem::path& directory, std::vector<fs::Entry> entries,
              std::string app_id = {});

    // Opens a terminal (terminal_alone()) with its shell in directory: "Opening Documents with
    // foot…", or "No terminal found: set $TERMINAL".
    void open_terminal(const std::filesystem::path& directory);

    // config.toml's [terminal] changed: Environment::configured_terminal from now on.
    void set_terminal(std::vector<std::string> configured);

    // config.toml's [open] programs: false, programs open as any other file (with their
    // default application, usually none). Followed live.
    void set_programs(bool allowed) noexcept { programs_ = allowed; }

    [[nodiscard]] bool programs() const noexcept { return programs_; }

    void set_ask_to_run(AskToRun ask) { ask_ = std::move(ask); }

    // Gives program u+x, then starts it (the user said yes).
    void allow_and_run(const std::filesystem::path& program);

    // The applications for entry (its type detected if unknown), worked out on a worker and
    // handed to done on the loop thread.
    void choices(const std::filesystem::path& directory, fs::Entry entry,
                 std::function<void(std::vector<AppChoice>)> done, bool all_installed = false);

    // Saves a default for a homogeneous selection, then uses the normal opening path.
    // Mixed-type/unknown selections are refused without changing associations.
    void set_default_and_open(const std::filesystem::path& directory,
                              std::vector<fs::Entry> entries, std::string app_id);

    // A process that ends with an error sooner than this after it started is reported: the
    // application could not open the file. Later exits are its own business.
    static constexpr std::chrono::seconds kQuickExit{2};

    [[nodiscard]] Launcher& launcher() noexcept { return launcher_; }

    // A plan is being worked out (tests).
    [[nodiscard]] bool planning() const noexcept { return planning_ > 0; }

private:
    void allow_and_run(const std::filesystem::path& program, std::shared_ptr<int> pinned);
    void start(OpenPlan plan);
    void start_one(const OpenPlan::Launch& launch, const std::string& token);

    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> database_;
    std::shared_ptr<const Environment> environment_;
    Report report_;
    TokenRequest token_;
    AskToRun ask_;
    bool programs_{true};
    Launcher launcher_;
    std::size_t planning_{0};
    // False once destroyed: plans and tokens still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::platform
