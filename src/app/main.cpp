#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/file_picker.hpp"
#include "rummage/core/file_watcher.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/log.hpp"
#include "rummage/core/memory.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/platform/about.hpp"
#include "rummage/platform/action_handoff.hpp"
#include "rummage/platform/applications.hpp"
#include "rummage/platform/browser_instance.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/file_manager.hpp"
#include "rummage/platform/file_manager_config.hpp"
#include "rummage/platform/file_picker_portal.hpp"
#include "rummage/platform/gvfs.hpp"
#include "rummage/platform/launcher.hpp"
#include "rummage/platform/opener.hpp"
#include "rummage/platform/portal.hpp"
#include "rummage/platform/program_output.hpp"
#include "rummage/platform/terminal_navigation.hpp"
#include "rummage/platform/udisks.hpp"
#include "rummage/theme/app_icon.hpp"
#include "rummage/theme/theme.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/activation.hpp"
#include "rummage/ui/clipboard.hpp"
#include "rummage/ui/display.hpp"
#include "rummage/ui/egl_context.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/ui/window.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_picker.hpp"
#include "rummage/views/state.hpp"
#include "rummage/views/title_bar.hpp"
#include "test_scene.hpp"
#include "window_session.hpp"

namespace {

namespace core = rummage::core;
namespace engines = rummage::engines;
namespace isolate = rummage::isolate;
namespace fs = rummage::fs;
namespace i18n = rummage::i18n;
namespace log = rummage::log;
namespace platform = rummage::platform;
namespace theme = rummage::theme;
namespace thumbs = rummage::thumbs;
namespace ui = rummage::ui;
namespace views = rummage::views;

// SIGINT/SIGTERM delivered as a readable fd, so Ctrl+C ends the loop and runs every
// destructor instead of killing the process mid-flight; SIGUSR1 asks for the memory report.
class SignalFd {
public:
    SignalFd() : mask_{block_signals()}, fd_{signalfd(-1, &mask_, SFD_CLOEXEC | SFD_NONBLOCK)} {
        if (fd_ < 0) {
            throw std::runtime_error{"signalfd failed"};
        }
    }

    ~SignalFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    SignalFd(const SignalFd&) = delete;
    SignalFd& operator=(const SignalFd&) = delete;
    SignalFd(SignalFd&&) = delete;
    SignalFd& operator=(SignalFd&&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    static sigset_t block_signals() {
        sigset_t mask{};
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGUSR1);
        // Writing to a program that stopped reading the clipboard: EPIPE, not death. Started
        // applications get an empty mask (Launcher).
        sigaddset(&mask, SIGPIPE);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        return mask;
    }

    sigset_t mask_;
    int fd_;
};

// Start timing (ROADMAP 7.6). rummage-bench-start puts the steady-clock time of its exec, in
// nanoseconds, in RUMMAGE_STARTUP_NS; every step then prints "startup: STEP MS" on stderr, the
// milliseconds since that exec. With RUMMAGE_STARTUP_QUIT set, rummage closes once its folder is
// shown. Without RUMMAGE_STARTUP_NS nothing is measured.
class StartupTrace {
public:
    StartupTrace() {
        const char* start = std::getenv("RUMMAGE_STARTUP_NS");  // NOLINT(concurrency-mt-unsafe)
        if (start == nullptr) {
            return;
        }
        const std::string_view text{start};
        std::int64_t nanoseconds = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), nanoseconds);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return;
        }
        start_ = std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanoseconds}};
        const char* quit = std::getenv("RUMMAGE_STARTUP_QUIT");  // NOLINT(concurrency-mt-unsafe)
        quit_ = quit != nullptr && *quit != '\0';
    }

    [[nodiscard]] bool active() const noexcept { return start_.has_value(); }

    [[nodiscard]] bool quit_when_shown() const noexcept { return quit_; }

    void mark(std::string_view step) const {
        if (!start_) {
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *start_)
                .count();
        std::fprintf(stderr, "startup: %.*s %.2f ms\n", static_cast<int>(step.size()), step.data(),
                     ms);
    }

private:
    std::optional<std::chrono::steady_clock::time_point> start_;
    bool quit_{false};
};

// "Ctrl+Shift+a  text 'A'  (repeat)": what the test scene shows for the last key.
std::string describe(const ui::KeyEvent& event) {
    std::string out;
    const ui::Modifiers& mods = event.modifiers;
    for (const auto& [active, name] :
         {std::pair{mods.ctrl, "Ctrl+"}, std::pair{mods.alt, "Alt+"},
          std::pair{mods.shift, "Shift+"}, std::pair{mods.super, "Super+"}}) {
        if (active) {
            out += name;
        }
    }
    std::array<char, 64> name{};
    xkb_keysym_get_name(event.keysym, name.data(), name.size());
    out += name.data();
    if (!event.text.empty()) {
        out += std::format("   text \u201c{}\u201d", event.text);
    }
    if (event.repeat) {
        out += "   (repeat)";
    }
    return out;
}

const char* button_name(ui::PointerButton button) {
    switch (button) {
        case ui::PointerButton::left:
            return "left";
        case ui::PointerButton::right:
            return "right";
        case ui::PointerButton::middle:
            return "middle";
        case ui::PointerButton::back:
            return "back";
        case ui::PointerButton::forward:
            return "forward";
        case ui::PointerButton::other:
            break;
    }
    return "other";
}

constexpr const char* kUsage =
    "Picker: rummage --pick|--pick-directory|--save-file [--multiple] [--name NAME] [--title "
    "TITLE] [DIR]\n"
    "Selected absolute paths are NUL-separated on stdout; exit 1 cancels.\n"
    "Portal backend: rummage --portal\n"
    "Show in Folder service: rummage --file-manager1\n"
    "Desktop integration: rummage --set-file-manager | --unset-file-manager\n"
    "Reveal items: rummage --reveal|--properties [--] PATH...\n"
    "Open folder tabs: rummage --open-folders [--] DIR...\n\n"
    "usage: rummage [--config FILE] [--check-config] [--test-scene] [--version] [--about] [DIR]\n"
    "  DIR             the folder to show (default: your home folder)\n"
    "  --new-window    open a separate browser window in the running instance\n"
    "  --config FILE   read FILE instead of ~/.config/rummage/config.toml\n"
    "  --test-scene    show the renderer test scene instead of a folder\n"
    "  --check-config  report problems in the config file and exit (status 1 if any)\n"
    "  --version       print the version and exit\n"
    "  --about         print what rummage runs on, for a bug report, and exit\n";

// The folder to open: DIR as given (made absolute, ".." resolved), else $HOME, else "/".
std::filesystem::path start_folder(const std::filesystem::path& given) {
    if (fs::is_trash(given))
        return given;
    std::filesystem::path folder = given;
    if (folder.empty()) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): startup
        folder = home != nullptr && home[0] != '\0' ? home : "/";
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(folder, error);
    return error ? folder : absolute;
}

// RUMMAGE_FORCE_CSD set and not "0": rummage's own title bar even where the compositor draws one.
bool client_decorations_forced() {
    const char* value = std::getenv("RUMMAGE_FORCE_CSD");  // NOLINT(concurrency-mt-unsafe): startup
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

// Consume before any worker starts; launched children must get their own activation tokens.
std::string take_activation_token() {
    std::string token;
    for (const char* name : {"XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"}) {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): startup
        if (value != nullptr && *value != '\0' && token.empty())
            token = value;
        unsetenv(name);  // NOLINT(concurrency-mt-unsafe): startup, before threads
    }
    return token;
}

int change_file_manager(bool set) {
    const auto environment = platform::Environment::from_environment();
    platform::ManagerChange change;
    if (!set) {
        change = platform::unset_file_manager(environment);
    } else {
        std::error_code error;
        const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
        const auto program = platform::file_manager_program(self);
        if (error || !program) {
            change = {.ok = false, .message = "cannot locate the rummage executable\n"};
        } else {
            const auto mime = fs::MimeDatabase::load();
            const auto apps = platform::Applications::load(environment, *mime);
            const auto* folders = apps.default_for("inode/directory", *mime);
            change = platform::set_file_manager(
                environment, *program, platform::bus_name_owner(platform::kFileManagerBusName),
                folders ? std::string_view{folders->id} : std::string_view{});
        }
    }
    std::fputs(change.message.c_str(), change.ok ? stdout : stderr);
    return change.ok ? 0 : 1;
}

// --about: what rummage runs on, one "Label: value" line each, to paste into a bug report.
int print_about() {
    std::vector<platform::AboutLine> lines;
    const std::string_view commit = RUMMAGE_COMMIT;
    lines.push_back({.label = "rummage",
                     .value = commit.empty() ? std::string{RUMMAGE_VERSION}
                                             : std::format("{} ({})", RUMMAGE_VERSION, commit)});
    const std::string build = platform::build_id();
    lines.push_back({.label = "Build", .value = build.empty() ? "unknown" : build});
    std::ranges::move(platform::system_about(), std::back_inserter(lines));
    // The engines found, and where their programs are: an installation in one line.
    const std::filesystem::path directory = engines::default_directory();
    const engines::Registry registry = engines::Registry::load_installed();
    std::string found;
    for (const engines::Engine& engine : registry.engines()) {
        found += std::format("{}{} ({})", found.empty() ? "" : ", ", engine.manifest.name,
                             engine.program.string());
    }
    if (found.empty()) {
        found = directory.empty() ? "none found" : std::format("none in {}", directory.string());
    }
    lines.push_back({.label = "Engines", .value = found});
    // The GPU as OpenGL names it, from a context without a window.
    try {
        const ui::Offscreen offscreen{1, 1};
        const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));  // NOLINT
        if (name != nullptr && *name != '\0') {
            lines.push_back({.label = "Graphics", .value = name});
        }
    } catch (const std::exception& error) {
        lines.push_back({.label = "Graphics", .value = std::format("unknown ({})", error.what())});
    }
    std::size_t width = 0;
    for (const platform::AboutLine& line : lines) {
        width = std::max(width, line.label.size());
    }
    for (const platform::AboutLine& line : lines) {
        std::printf("%-*s %s\n", static_cast<int>(width + 1), (line.label + ":").c_str(),
                    line.value.c_str());
    }
    return 0;
}

// --check-config: every problem on stderr, a one-line verdict on stdout.
int check_config(const std::filesystem::path& path, const core::ConfigResult& result) {
    for (const std::string& problem : result.problems) {
        std::fprintf(stderr, "%s\n", problem.c_str());
    }
    if (!result.problems.empty()) {
        std::printf("%s: %zu problem(s)\n", path.c_str(), result.problems.size());
        return 1;
    }
    std::printf("%s: %s\n", path.c_str(), result.from_file ? "ok" : "not found, defaults in use");
    return 0;
}

// What gvfs asks before a mount, answered on the Browser's cards.
platform::Gvfs::Prompts remote_prompts(views::Remotes::Prompts prompts) {
    using platform::PasswordRequest;
    return {
        .password =
            [login = std::move(prompts.login)](
                const PasswordRequest& request,
                std::function<void(const platform::PasswordAnswer&)> answer) {
                if (!login) {
                    answer({});
                    return;
                }
                const auto asks = [&request](std::uint32_t flag) {
                    return (request.flags & flag) != 0;
                };
                login({.message = request.message,
                       .user = request.user,
                       .domain = request.domain,
                       .ask_user = asks(PasswordRequest::kNeedUsername),
                       .ask_domain = asks(PasswordRequest::kNeedDomain),
                       .ask_password = asks(PasswordRequest::kNeedPassword),
                       .anonymous = asks(PasswordRequest::kAnonymousSupported),
                       .saving = asks(PasswordRequest::kSavingSupported)},
                      [answer = std::move(answer)](const std::optional<views::Login>& given) {
                          platform::PasswordAnswer out;
                          if (given) {
                              out = {.aborted = false,
                                     .anonymous = given->anonymous,
                                     .user = given->user,
                                     .domain = given->domain,
                                     .password = given->password,
                                     .remember = given->remember};
                          }
                          answer(out);
                      });
            },
        .question = std::move(prompts.question),
        .cancel = std::move(prompts.cancel),
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view{argv[1]} == "--terminal-navigation-session")
        return platform::run_terminal_navigation(argv[2], argv[3], argv[4]);
    core::give_back_large_blocks();  // before any thread: decoded photos go back to the system
    // System error texts (strerror, std::error_code) in the user's language, as the interface;
    // only messages: numbers and sorting stay as they are.
    std::setlocale(LC_MESSAGES, "");
    const StartupTrace startup;
    startup.mark("main");
    std::filesystem::path config_path;
    std::filesystem::path start_dir;
    bool check_only = false;
    bool test_scene = false;
    bool portal_backend = false;
    bool file_manager_backend = false;
    std::optional<bool> configure_file_manager;
    bool reveal = false;
    bool reveal_details = false;
    bool open_folders = false;
    bool new_window = false;
    bool only_paths = false;
    std::vector<std::filesystem::path> requested_paths;
    bool picker_channel = false;
    std::optional<core::PickerRequest> picker_request;
    core::PickerResult picker_result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (only_paths || ((reveal || open_folders) && !arg.starts_with("-"))) {
            if (reveal || open_folders)
                requested_paths.emplace_back(arg);
            else if (start_dir.empty())
                start_dir = arg;
            else {
                std::fprintf(stderr, "rummage: only one folder without --open-folders\n");
                return 2;
            }
            continue;
        }
        if (arg == "--") {
            only_paths = true;
            continue;
        }
        if (arg == "--reveal" || arg == "--properties") {
            reveal = true;
            reveal_details = reveal_details || arg == "--properties";
            continue;
        }
        if (arg == "--new-window") {
            new_window = true;
            continue;
        }
        if (arg == "--open-folders") {
            open_folders = true;
            continue;
        }
        if (arg == "--file-manager1") {
            file_manager_backend = true;
            continue;
        }
        if (arg == "--set-file-manager" || arg == "--unset-file-manager") {
            if (configure_file_manager) {
                std::fprintf(stderr, "rummage: choose one file manager configuration command\n");
                return 2;
            }
            configure_file_manager = arg == "--set-file-manager";
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::puts(kUsage);
            return 0;
        }
        if (arg == "--version") {
            std::puts("rummage " RUMMAGE_VERSION);
            return 0;
        }
        if (arg == "--about") {
            return print_about();
        }
        if (arg == "--portal") {
            portal_backend = true;
        } else if (arg == "--picker-channel") {
            picker_channel = true;
            try {
                picker_request = core::read_picker_request(3);
                close(3);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "rummage: %s\n", e.what());
                return 2;
            }
        } else if (arg == "--pick" || arg == "--pick-directory" || arg == "--save-file") {
            if (picker_request) {
                std::fprintf(stderr, "rummage: choose one picker mode\n");
                return 2;
            }
            picker_request.emplace();
            picker_request->directory = arg == "--pick-directory";
            picker_request->mode = arg == "--save-file" ? core::PickerRequest::Mode::save
                                                        : core::PickerRequest::Mode::open;
        } else if (arg == "--multiple" && picker_request) {
            picker_request->multiple = true;
        } else if ((arg == "--name" || arg == "--title") && picker_request && i + 1 < argc) {
            (arg == "--name" ? picker_request->name : picker_request->title) = argv[++i];
        } else if (arg == "--check-config") {
            check_only = true;
        } else if (arg == "--test-scene") {
            test_scene = true;
        } else if (arg == "--config") {
            if (i + 1 == argc) {
                std::fprintf(stderr, "rummage: --config needs a file\n%s", kUsage);
                return 2;
            }
            config_path = argv[++i];
        } else if (!arg.starts_with("-") && start_dir.empty()) {
            start_dir = arg;
        } else {
            std::fprintf(stderr, "rummage: unknown option '%s'\n%s", argv[i], kUsage);
            return 2;
        }
    }
    const int modes = static_cast<int>(portal_backend) + static_cast<int>(file_manager_backend) +
                      static_cast<int>(configure_file_manager.has_value()) +
                      static_cast<int>(reveal) + static_cast<int>(open_folders) +
                      static_cast<int>(picker_request.has_value()) + static_cast<int>(test_scene) +
                      static_cast<int>(check_only);
    if ((new_window && (portal_backend || file_manager_backend || configure_file_manager ||
                        picker_request || test_scene || check_only)) ||
        modes > 1 ||
        ((reveal || open_folders) && (requested_paths.empty() || !start_dir.empty())) ||
        ((portal_backend || file_manager_backend || configure_file_manager) &&
         !start_dir.empty())) {
        std::fprintf(stderr, "rummage: incompatible launch modes or missing paths\n");
        return 2;
    }
    if (configure_file_manager)
        return change_file_manager(*configure_file_manager);
    for (auto& path : requested_paths) {
        std::error_code error;
        path = std::filesystem::absolute(path, error).lexically_normal();
        if (error) {
            std::fprintf(stderr, "rummage: cannot resolve path\n");
            return 2;
        }
        if (!path.has_filename() && path.has_relative_path())
            path = path.parent_path();
    }
    if (reveal || open_folders) {
        platform::BrowserRequest checked;
        for (const auto& path : requested_paths)
            checked.paths.push_back(path.string());
        if (!platform::valid_browser_request(checked)) {
            std::fprintf(stderr, "rummage: launch request exceeds path limits\n");
            return 2;
        }
        std::erase_if(requested_paths, [open_folders](const auto& path) {
            std::error_code error;
            return open_folders
                       ? !std::filesystem::is_directory(path, error)
                       : !std::filesystem::exists(std::filesystem::symlink_status(path, error));
        });
        if (requested_paths.empty()) {
            std::fprintf(stderr, "rummage: no existing paths to show\n");
            return 1;
        }
    }
    std::string startup_token = take_activation_token();
    if ((portal_backend && (picker_request || test_scene || check_only || !start_dir.empty())) ||
        (picker_request && (test_scene || check_only)) ||
        (picker_request && picker_request->mode != core::PickerRequest::Mode::open &&
         picker_request->multiple)) {
        std::fprintf(stderr, "rummage: incompatible picker options\n");
        return 2;
    }
    if (picker_request && !picker_request->folder.empty()) {
        std::error_code error;
        if (std::filesystem::is_directory(picker_request->folder, error)) {
            start_dir = picker_request->folder;
        }
    }
    std::error_code config_creation_error;
    std::vector<std::string> migration_errors;
    if (config_path.empty()) {
        config_path = core::default_config_path();
        if (!check_only && !test_scene && !portal_backend && !file_manager_backend &&
            !picker_request) {
            bool config_migrated = true;
            for (const auto& path :
                 {config_path, theme::default_colors_path(), views::default_state_path()}) {
                if (const auto error = core::migrate_nordend_file(path); !error.empty()) {
                    migration_errors.push_back(path.string() + ": " + error);
                    if (path == config_path)
                        config_migrated = false;
                }
            }
            if (config_migrated)
                config_creation_error = core::create_default_config(config_path);
        }
    }
    core::ConfigResult config_result = core::load_config(config_path);
    auto configured_actions = config_result.config.actions;
    const auto binding_problems =
        views::configure_shortcuts(config_result.config.keybindings, configured_actions);
    for (const auto& problem : binding_problems) {
        config_result.problems.push_back(problem);
    }
    if (!binding_problems.empty()) {
        configured_actions.clear();
    }
    if (check_only) {
        return check_config(config_path, config_result);
    }

    log::init_from_env();
    log::info("rummage {}", RUMMAGE_VERSION);
    for (const auto& error : migration_errors)
        log::warn("Nordend settings migration: {}", error);
    if (config_creation_error) {
        log::warn("config: cannot create {}: {}; continuing with defaults", config_path.string(),
                  config_creation_error.message());
    }
    if (config_result.from_file) {
        log::info("config: {}", config_path.string());
    } else {
        log::info("config: {} not used, defaults in use", config_path.string());
    }
    for (const std::string& problem : config_result.problems) {
        log::warn("config: {}", problem);
    }
    const core::Config& config = config_result.config;

    try {
        // Before any other thread exists, so every thread inherits the blocked signals.
        const SignalFd signals;
        core::EventLoop loop;
        // Set once the browser exists: what it holds, for the memory report.
        std::function<std::vector<std::string>()> memory_lines;
        const ui::Renderer* renderer_for_stats = nullptr;  // window owns it through loop.run()
        loop.add_fd(signals.fd(), POLLIN, [&](short /*revents*/) {
            signalfd_siginfo info{};
            if (read(signals.fd(), &info, sizeof info) != sizeof info) {
                return;
            }
            if (info.ssi_signo == SIGPIPE) {
                return;  // a clipboard reader went away: the write already failed with EPIPE
            }
            if (info.ssi_signo == SIGUSR1) {
                const auto motion_stats = renderer_for_stats ? renderer_for_stats->motion_stats()
                                                             : ui::Renderer::MotionStats{};
                log::info(
                    "motion: {} bytes, {} allocations, {} passes, {} fallbacks; CPU frame last "
                    "{:.3f} ms, max {:.3f} ms ({} frames)",
                    motion_stats.texture_bytes, motion_stats.allocations, motion_stats.passes,
                    motion_stats.fallbacks, motion_stats.last_frame_ms, motion_stats.max_frame_ms,
                    motion_stats.frames);
                // A development report (ROADMAP 6.3b): pkill -USR1 rummage.
                for (const std::string& line : core::describe(core::read_memory_report())) {
                    log::info("memory: {}", line);
                }
                for (const std::string& line :
                     memory_lines ? memory_lines() : std::vector<std::string>{}) {
                    log::info("memory: {}", line);
                }
                return;
            }
            log::info("signal received, closing");
            loop.quit(0);
        });

        if (file_manager_backend) {
            return platform::run_file_manager(loop);
        }
        if (portal_backend) {
            return platform::run_file_picker_portal(loop);
        }

        std::string instance_name;
        platform::BrowserRequest launch_request;
        const bool reuse = !picker_request && !test_scene && config.windows.reuse;
        if (reuse) {
            const char* display_name =
                std::getenv("WAYLAND_DISPLAY");                    // NOLINT(concurrency-mt-unsafe)
            const char* runtime = std::getenv("XDG_RUNTIME_DIR");  // NOLINT(concurrency-mt-unsafe)
            std::filesystem::path display_path = display_name ? display_name : "wayland-0";
            if (display_path.is_relative() && runtime)
                display_path = std::filesystem::path{runtime} / display_path;
            instance_name = platform::browser_instance_name(
                std::filesystem::absolute(config_path).lexically_normal().string(),
                display_path.lexically_normal().string());
            launch_request.show =
                reveal ? (reveal_details ? platform::Show::properties : platform::Show::items)
                       : platform::Show::folders;
            for (const auto& path : requested_paths)
                launch_request.paths.push_back(path.string());
            if (launch_request.paths.empty()) {
                const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
                const auto path = start_dir.empty()
                                      ? std::filesystem::path{home && *home ? home : "/"}
                                      : start_dir;
                launch_request.paths.push_back(
                    fs::is_trash(path)
                        ? path.string()
                        : std::filesystem::absolute(path).lexically_normal().string());
            }
            launch_request.token = startup_token;
            launch_request.new_window = new_window;
            const auto forwarded = platform::forward_browser_request(instance_name, launch_request);
            if (forwarded != platform::BrowserForward::unavailable)
                return forwarded == platform::BrowserForward::accepted ? 0 : 1;
        }
        ui::Display display;
        startup.mark("display");
        ui::EglContext egl{display};
        startup.mark("egl");
        ui::FontSystem fonts{config.font.family, config.font.monospace};
        startup.mark("fonts");
        theme::Theme theme{theme::default_colors_path()};
        theme.watch(loop);
        const auto apply_theme = [&](const core::Config::Appearance& appearance) {
            using Source = theme::Theme::Source;
            theme.set_source(appearance.palette_source == "desktop"   ? Source::desktop
                             : appearance.palette_source == "file"    ? Source::file
                             : appearance.palette_source == "kde"     ? Source::kde
                             : appearance.palette_source == "builtin" ? Source::builtin
                                                                      : Source::automatic);
            theme.set_dark_override(appearance.color_scheme == "system"
                                        ? std::nullopt
                                        : std::optional<bool>{appearance.color_scheme == "dark"});
        };
        apply_theme(config.appearance);
        // Keep desktop preferences current even when another palette source is selected.
        std::unique_ptr<platform::DesktopPortal> portal;
        std::function<void(bool)> motion_preference;
        const auto follow_desktop = [&] {
            if (!portal->available()) {
                return;
            }
            const platform::Appearance& appearance = portal->appearance();
            if (motion_preference)
                motion_preference(appearance.reduced_motion);
            theme::Desktop desktop{.dark = appearance.dark,
                                   .accent = std::nullopt,
                                   .high_contrast = appearance.high_contrast};
            if (appearance.accent) {
                const auto [r, g, b] = *appearance.accent;
                desktop.accent = ui::Color{static_cast<float>(r), static_cast<float>(g),
                                           static_cast<float>(b), 1.0F};
            }
            theme.set_desktop(desktop);
        };
        portal = std::make_unique<platform::DesktopPortal>(loop, follow_desktop);
        follow_desktop();
        startup.mark("portal");
        // How rummage was left (ROADMAP 7.5a).
        const std::filesystem::path state_path =
            views::default_state_path(picker_request.has_value());
        views::StateResult remembered = views::load_state(state_path);
        if (std::error_code error; picker_request && !std::filesystem::exists(state_path, error)) {
            remembered.state.mode = views::State::Mode::grid;
        }
        for (const std::string& problem : remembered.problems) {
            log::warn("state: {}", problem);
        }
        ui::Seat seat{display};
        seat.set_multi_click(std::chrono::milliseconds{config.pointer.double_click_ms},
                             static_cast<float>(config.pointer.double_click_distance));
        display.attach(loop);
        seat.attach(loop);

        core::WorkerPool pool{loop};
        startup.mark("workers");
        const auto mime = fs::MimeDatabase::load();
        startup.mark("mime");
        // Pictures are made by the engines in their cages (ROADMAP 2b, 3a), never here.
        {
            auto registry =
                std::make_shared<const engines::Registry>(engines::Registry::load_installed());
            isolate::use_engines(*registry, {}, mime);
            engines::set_installed(std::move(registry));
        }
        startup.mark("engines");
        // rummage's icon in the desktop's accent (ROADMAP 9.2), written on a worker.
        bool follow_accent = config.icon.follow_accent;
        theme::AppIconKeeper app_icon{pool, theme::default_app_icon_place()};
        const auto update_icon = [&] {
            app_icon.follow(follow_accent ? theme.accent() : std::nullopt);
        };
        update_icon();
        fs::DirectoryWatcher watcher{loop};
        ui::IconSet icons{fonts};
        icons.set_theme(config.icon.theme);
        icons.set_place_icons(config.icon.places);
        startup.mark("icons");
        ui::Activation activation{display, loop};
        ui::Clipboard clipboard{display, loop};  // before the browser that uses it
        views::StateKeeper keeper{loop, pool, state_path, remembered.state};
        auto thumbnail_cache =
            std::make_shared<thumbs::Cache>(thumbs::default_root(), "rummage-" RUMMAGE_VERSION);

        struct Binding {
            ui::Window* window{};
            views::Browser* browser{};
            std::weak_ptr<Binding> parent;
            std::function<void(const ui::KeyEvent&)> key;
            std::function<void(const ui::PointerEvent&)> pointer;
            std::function<void(const core::ConfigResult&)> config;
            std::function<void()> save;
        };

        std::vector<std::shared_ptr<Binding>> bindings;
        std::weak_ptr<Binding> last_browser, state_owner, drag_origin;
        memory_lines = [&] {
            std::vector<std::string> lines;
            for (const auto& binding : bindings)
                if (binding->browser) {
                    auto held = binding->browser->memory_lines();
                    lines.insert(lines.end(), held.begin(), held.end());
                }
            return lines;
        };
        const auto find_window = [&](wl_surface* surface) -> std::shared_ptr<Binding> {
            if (!surface)
                return {};
            for (const auto& binding : bindings)
                if (binding->window && binding->window->surface() == surface)
                    return binding;
            return {};
        };
        seat.set_key_handler([&](const ui::KeyEvent& event) {
            if (const auto binding = find_window(seat.keyboard_surface()); binding && binding->key)
                binding->key(event);
        });
        seat.set_pointer_handler([&](const ui::PointerEvent& event) {
            if (const auto binding = find_window(seat.pointer_surface());
                binding && binding->pointer)
                binding->pointer(event);
        });
        seat.set_focus_handler([&](wl_surface* surface) {
            if (const auto binding = find_window(surface); binding && binding->browser) {
                last_browser = binding;
                state_owner = binding;
                if (binding->save)
                    binding->save();
            }
        });
        clipboard.set_lost_handler([&] {
            for (const auto& binding : bindings)
                if (binding->browser)
                    binding->browser->clipboard_lost();
        });
        clipboard.set_drop_handler([&](const ui::DropEvent& event) {
            if (const auto binding = find_window(clipboard.drop_surface());
                binding && binding->browser) {
                const auto origin = drag_origin.lock();
                if (event.kind == ui::DropEvent::Kind::enter && origin && origin != binding &&
                    origin->browser)
                    binding->browser->adopt_drag(*origin->browser);
                binding->browser->drop_event(binding->window->content_map().to_content(event));
                if (event.kind == ui::DropEvent::Kind::leave && origin != binding)
                    binding->browser->drag_ended();
            }
        });
        clipboard.set_drag_end_handler([&](bool /*dropped*/) {
            if (const auto origin = drag_origin.lock(); origin && origin->window)
                origin->window->drop_drag_icon();
            drag_origin.reset();
            for (const auto& binding : bindings)
                if (binding->browser)
                    binding->browser->drag_ended();
        });
        theme.set_change_handler([&] {
            for (const auto& binding : bindings)
                binding->window->set_background(theme.palette().surface);
            update_icon();
        });
        icons.watch_system_theme(loop, [&] {
            for (const auto& binding : bindings)
                binding->window->request_redraw();
        });
        motion_preference = [&](bool reduced) {
            for (const auto& binding : bindings)
                if (binding->browser)
                    binding->browser->set_reduced_motion(reduced);
        };
        std::unique_ptr<fs::PlacesWatcher> places;
        std::unique_ptr<platform::Udisks> udisks;
        std::unique_ptr<platform::Gvfs> gvfs;
        const auto show_places = [&] {
            if (!places)
                return;
            auto shown = udisks ? platform::with_volumes(places->places(), udisks->volumes())
                                : places->places();
            if (gvfs)
                shown = platform::with_remote_mounts(std::move(shown), gvfs->mounts());
            for (const auto& binding : bindings)
                if (binding->browser)
                    binding->browser->set_places(shown);
        };
        if (!test_scene) {
            places = std::make_unique<fs::PlacesWatcher>(loop, fs::PlaceSources::from_environment(),
                                                         show_places);
            udisks = std::make_unique<platform::Udisks>(loop, show_places);
            gvfs = std::make_unique<platform::Gvfs>(loop, show_places);
        }
        const auto update_config = [&](const core::ConfigResult& fresh) {
            const auto previous = config_result;
            config_result = fresh;
            if (fresh.actions_valid && fresh.context_menu_valid &&
                views::configure_shortcuts(fresh.config.keybindings, fresh.config.actions)
                    .empty()) {
                configured_actions = fresh.config.actions;
            } else {
                config_result.config.actions = previous.config.actions;
                config_result.config.keybindings = previous.config.keybindings;
                config_result.config.context_menu = previous.config.context_menu;
            }
            apply_theme(fresh.config.appearance);
            icons.set_theme(fresh.config.icon.theme);
            icons.set_place_icons(fresh.config.icon.places);
            seat.set_multi_click(std::chrono::milliseconds{fresh.config.pointer.double_click_ms},
                                 static_cast<float>(fresh.config.pointer.double_click_distance));
            follow_accent = fresh.config.icon.follow_accent;
            update_icon();
            for (const auto& binding : bindings)
                if (binding->config)
                    binding->config(fresh);
        };
        std::unique_ptr<core::FileWatcher> config_watcher;
        if (!config_path.empty()) {
            config_watcher = std::make_unique<core::FileWatcher>(loop, config_path, [&] {
                const auto fresh = core::load_config(config_path);
                for (const auto& problem : fresh.problems)
                    log::warn("config: {}", problem);
                if (!fresh.from_file && !fresh.problems.empty())
                    return;
                update_config(fresh);
            });
        }
        std::function<rummage::app::WindowSession(std::filesystem::path, std::string, bool)>
            open_browser;
        std::function<rummage::app::WindowSession(std::shared_ptr<Binding>, std::filesystem::path,
                                                  bool, bool, std::string, bool, std::string)>
            open_auxiliary;
        std::weak_ptr<Binding> settings_window;
        std::vector<rummage::app::WindowSession> sessions;
        const auto reap = [&] {
            for (const auto& session : sessions)
                session.rethrow();
            std::erase_if(sessions, [](const auto& session) { return session.done(); });
            if (sessions.empty())
                loop.quit();
        };

        struct Registration {
            std::function<void()> remove;

            ~Registration() { remove(); }
        };

        const auto remove_binding = [&](const std::shared_ptr<Binding>& binding) {
            if (drag_origin.lock() == binding)
                clipboard.cancel_drag();
            seat.forget_surface(binding->window->surface());
            for (const auto& other : bindings)
                if (other->parent.lock() == binding) {
                    other->window->set_parent(nullptr);
                    other->parent.reset();
                }
            if (last_browser.lock() == binding)
                last_browser.reset();
            binding->window = nullptr;
            binding->browser = nullptr;
            binding->key = {};
            binding->pointer = {};
            binding->config = {};
            binding->save = {};
            std::erase(bindings, binding);
            if (last_browser.expired())
                for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
                    if ((*it)->browser) {
                        last_browser = *it;
                        break;
                    }
            renderer_for_stats = nullptr;
        };
        const auto quit_windows = [&] {
            const auto closing = bindings;
            for (const auto& binding : closing)
                if (binding->window)
                    binding->window->close();
        };
        open_browser = [&](std::filesystem::path folder, std::string token,
                           bool initial) -> rummage::app::WindowSession {
            const auto window_state = initial ? remembered.state : views::State{};
            ui::Window window{
                display,
                egl,
                fonts,
                {.title = picker_request ? (picker_request->title.empty()
                                                ? std::string{i18n::tr("Select a file — rummage")}
                                                : picker_request->title)
                                         : "rummage",
                 .app_id = "rummage",
                 .width = window_state.window_width,
                 .height = window_state.window_height,
                 .maximized = window_state.maximized,
                 .background = theme.palette().surface,
                 .force_client_decorations = client_decorations_forced(),
                 .background_opacity = config.appearance.background_opacity,
                 .parent_window = picker_request ? picker_request->parent_window : "",
                 .modal = picker_request && picker_request->modal}};
            window.set_zoom(config.font.zoom());
            startup.mark("window");
            auto binding = std::make_shared<Binding>();
            binding->window = &window;
            bindings.push_back(binding);
            auto lifetime = std::make_shared<rummage::app::WindowLifetime>();
            // Registered before controls: even a constructor failure removes the surface route.
            Registration registration{[&] { remove_binding(binding); }};
            // The column view and the grid (ROADMAP 4.2, 4.4), unless the renderer test scene was
            // asked for.
            std::unique_ptr<views::Browser> columns;
            std::unique_ptr<platform::ProgramOutput> folder_chooser;
            // Picker destruction cancels its terminal session; the session must outlive it.
            std::unique_ptr<platform::TerminalNavigation> terminal_navigation;
            std::unique_ptr<views::FilePicker> picker;
            // config.toml is followed while rummage runs: the double click changes at once, the
            // fonts at the next start (`config` keeps the ones in use).
            std::unique_ptr<platform::Opener> opener;
            const auto configured_terminal = [](const core::Config& from) {
                std::vector<std::string> line;
                if (!from.terminal.program.empty()) {
                    line.push_back(from.terminal.program);
                    line.insert(line.end(), from.terminal.args.begin(), from.terminal.args.end());
                }
                return line;
            };
            auto picker_terminal = configured_terminal(config);
            auto sidebar_config = config.sidebar;
            const auto configure_auxiliary = [&] {
                if (picker_request || !columns)
                    return;
                const auto properties = [&, weak = std::weak_ptr{binding}](
                                            const std::filesystem::path& path, bool editable,
                                            bool drive, const std::string& label) {
                    activation.request(
                        display.seat(), seat.input_serial(), window.surface(),
                        [&, weak, path, editable, drive, label](std::string activation_token) {
                            loop.post([&, weak, path, editable, drive, label,
                                       activation_token = std::move(activation_token)]() mutable {
                                sessions.push_back(open_auxiliary(weak.lock(), path, editable,
                                                                  drive, label, false,
                                                                  std::move(activation_token)));
                                reap();
                            });
                        });
                };
                std::function<void()> settings;
                if (config.windows.separate_settings)
                    settings = [&, weak = std::weak_ptr{binding}] {
                        activation.request(
                            display.seat(), seat.input_serial(), window.surface(),
                            [&, weak](std::string activation_token) {
                                loop.post(
                                    [&, weak,
                                     activation_token = std::move(activation_token)]() mutable {
                                        if (const auto existing = settings_window.lock();
                                            existing && existing->window)
                                            activation.activate(activation_token,
                                                                existing->window->surface());
                                        else {
                                            sessions.push_back(
                                                open_auxiliary(weak.lock(), {}, false, false, {},
                                                               true, std::move(activation_token)));
                                            reap();
                                        }
                                    });
                            });
                    };
                columns->set_auxiliary_windows(std::move(settings), properties);
            };
            const auto apply_config = [&](const core::ConfigResult& loaded) {
                const auto& fresh = loaded.config;
                picker_terminal = configured_terminal(fresh);
                configure_auxiliary();
                window.set_zoom(fresh.font.zoom());
                if (columns) {
                    columns->set_motion_config(fresh.animations);
                    columns->set_keyboard_config(fresh.keyboard);
                    columns->status_bar().set_text_size(fresh.status_bar.text_size);
                    columns->set_preview_in_sidebar(fresh.appearance.preview_position == "sidebar");
                    if (fresh.sidebar != sidebar_config) {
                        columns->set_sidebar_config(fresh.sidebar);
                        sidebar_config = fresh.sidebar;
                    }
                }
                if (loaded.actions_valid && loaded.context_menu_valid) {
                    const auto problems =
                        views::configure_shortcuts(fresh.keybindings, fresh.actions);
                    for (const auto& problem : problems) {
                        log::warn("config: {}; keeping previous actions, keybindings and menus",
                                  problem);
                    }
                    if (problems.empty() && columns) {
                        columns->set_actions(fresh.actions);
                        columns->set_context_menu(fresh.context_menu);
                    }
                } else {
                    log::warn("config: keeping previous actions, keybindings and menus");
                }
                window.request_redraw();
                apply_theme(fresh.appearance);
                icons.set_theme(fresh.icon.theme);
                icons.set_place_icons(fresh.icon.places);
                window.set_background_opacity(fresh.appearance.background_opacity);
                seat.set_multi_click(std::chrono::milliseconds{fresh.pointer.double_click_ms},
                                     static_cast<float>(fresh.pointer.double_click_distance));
                follow_accent = fresh.icon.follow_accent;
                update_icon();
                if (opener) {
                    opener->set_terminal(configured_terminal(fresh));
                    opener->set_programs(fresh.open.programs && !picker_request);
                }
            };
            // Writes state.toml as things change; the window's size is the one it has when neither
            // maximized, fullscreen nor tiled, so a maximized window comes back to it.
            std::int32_t restored_width = window_state.window_width;
            std::int32_t restored_height = window_state.window_height;
            const auto note_state = [&] {
                if (!columns || state_owner.lock() != binding)
                    return;
                const ui::WindowState& shown = window.state();
                if (!shown.maximized && !shown.fullscreen && !shown.tiled_left &&
                    !shown.tiled_right && !shown.tiled_top && !shown.tiled_bottom) {
                    restored_width = window.width();
                    restored_height = window.height();
                }
                views::State state = columns->state();
                state.window_width = restored_width;
                state.window_height = restored_height;
                state.maximized = shown.maximized;
                if (picker_request) {
                    state.tabs.clear();
                    state.shown_tab = 0;
                }
                keeper.note(state);
            };
            std::unique_ptr<platform::ActionHandoff> action_handoff;
            if (!test_scene) {
                columns = std::make_unique<views::Browser>(
                    loop, pool, watcher, mime, [&] { window.request_redraw(); },
                    [&](ui::CursorShape shape) { seat.set_cursor(shape); }, thumbnail_cache);
                binding->browser = columns.get();
                last_browser = binding;
                state_owner = binding;
                platform::Environment environment = platform::Environment::from_environment();
                environment.configured_terminal = configured_terminal(config);
                opener = std::make_unique<platform::Opener>(
                    loop, pool, mime, std::move(environment),
                    [&](std::string message, bool error) {
                        columns->show_message(std::move(message), error);
                    },
                    [&](std::function<void(std::string)> done) {
                        activation.request(display.seat(), seat.input_serial(), window.surface(),
                                           std::move(done));
                    });
                opener->set_programs(config.open.programs && !picker_request);
                opener->set_ask_to_run(
                    [&](const std::filesystem::path& program, std::function<void()> allow_and_run) {
                        columns->ask_to_run(program, std::move(allow_and_run));
                    });
                columns->set_picker_mode(picker_request.has_value());
                configure_auxiliary();
                if (!picker_request)
                    columns->set_new_window_handler(
                        [&, weak = std::weak_ptr{binding}](const std::filesystem::path& path) {
                            activation.request(
                                display.seat(), seat.input_serial(), window.surface(),
                                [&, weak, path](std::string activation_token) {
                                    if (const auto owner = weak.lock(); owner && owner->window)
                                        loop.post([&, path,
                                                   activation_token =
                                                       std::move(activation_token)]() mutable {
                                            sessions.push_back(open_browser(
                                                path, std::move(activation_token), false));
                                            reap();
                                        });
                                });
                        });
                columns->set_actions(configured_actions);
                columns->set_motion_config(config.animations);
                columns->set_keyboard_config(config.keyboard);
                columns->set_reduced_motion(portal->appearance().reduced_motion);
                columns->status_bar().set_text_size(config.status_bar.text_size);
                columns->set_preview_in_sidebar(config.appearance.preview_position == "sidebar");
                columns->set_context_menu(config.context_menu);
                action_handoff = std::make_unique<platform::ActionHandoff>(
                    loop, platform::Environment::from_environment(),
                    [&](std::string message, bool error) {
                        columns->show_action_result(std::move(message), error);
                    },
                    [&](std::function<void(std::string)> done) {
                        activation.request(display.seat(), seat.input_serial(), window.surface(),
                                           std::move(done));
                    });
                action_handoff->set_password_prompt(
                    [&](const std::filesystem::path& path, bool retry, auto done) {
                        columns->password_dialog().open_password(path.filename().string(), retry,
                                                                 std::move(done));
                    });
                columns->set_action_handoff(
                    [&](const core::Action& action, const core::ActionContext& context) {
                        action_handoff->run(action, context);
                    },
                    [&](const core::Action& action, const core::ActionContext& context) {
                        return action_handoff->available(action, context);
                    });
                columns->set_open_handler(
                    [&](const std::filesystem::path& directory, std::vector<fs::Entry> files) {
                        opener->open(directory, std::move(files));
                    });
                columns->set_open_with_handlers(
                    [&](const std::filesystem::path& directory, fs::Entry entry,
                        std::function<void(std::vector<views::Browser::AppChoice>)> done) {
                        opener->choices(
                            directory, std::move(entry),
                            [done = std::move(done)](std::vector<platform::AppChoice> found) {
                                std::vector<views::Browser::AppChoice> apps;
                                apps.reserve(found.size());
                                for (platform::AppChoice& app : found) {
                                    apps.push_back({.id = std::move(app.id),
                                                    .name = std::move(app.name),
                                                    .is_default = app.is_default});
                                }
                                done(std::move(apps));
                            });
                    },
                    [&](const std::filesystem::path& directory, std::vector<fs::Entry> files,
                        std::string app_id) {
                        opener->open(directory, std::move(files), std::move(app_id));
                    });
                columns->settings_dialog().set_theme_status_handler([&] { return theme.status(); });
                columns->set_settings_handler(
                    config_path,
                    [&] {
                        opener->open(config_path.parent_path(),
                                     {{.name = config_path.filename().string(),
                                       .kind = fs::EntryKind::file}});
                    },
                    update_config);
                columns->set_terminal_handler([&](const std::filesystem::path& directory) {
                    opener->open_terminal(directory);
                });
                if (!picker_request) {
                    columns->set_folder_chooser(
                        [&](const std::filesystem::path& start,
                            std::function<void(std::filesystem::path)> chosen) {
                            if (folder_chooser && folder_chooser->running())
                                return;
                            std::error_code error;
                            const auto self =
                                std::filesystem::read_symlink("/proc/self/exe", error);
                            folder_chooser = std::make_unique<platform::ProgramOutput>(
                                loop, [chosen = std::move(chosen)](const std::string& output,
                                                                   int status) {
                                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                                        return;
                                    const auto end = output.find('\0');
                                    if (end == std::string::npos)
                                        return;
                                    const std::filesystem::path path{output.substr(0, end)};
                                    if (path.is_absolute())
                                        chosen(path);
                                });
                            if (error || !folder_chooser->start(
                                             self, {"--pick-directory", "--title",
                                                    std::string{i18n::tr("Choose Folder")},
                                                    start.string()})) {
                                folder_chooser.reset();
                                columns->show_message(
                                    std::string{i18n::tr("The folder chooser could not start")},
                                    true);
                            }
                        });
                }
                columns->set_quit_handler(quit_windows);
                if (clipboard.available()) {
                    columns->set_system_clipboard({
                        .offer =
                            [&](std::vector<views::SystemClipboard::Format> formats) {
                                clipboard.offer(std::move(formats), seat.input_serial());
                            },
                        .clear = [&] { clipboard.clear(seat.input_serial()); },
                        .owned = [&] { return clipboard.owned(); },
                        .generation = [&] { return clipboard.generation(); },
                        .types = [&] { return clipboard.types(); },
                        .read =
                            [&](const std::string& type, views::SystemClipboard::Done done) {
                                clipboard.read(type, std::move(done));
                            },
                    });
                    // Drag and drop (ROADMAP 7.1) on the same wl_data_device.
                    columns->set_drag_and_drop({
                        .start =
                            [&](std::vector<views::DragAndDrop::Format> formats) {
                                drag_origin = binding;
                                wl_surface* const icon = window.make_drag_icon(
                                    views::Browser::kDragIconWidth, views::Browser::kDragIconHeight,
                                    [&](ui::Renderer& renderer, const ui::Rect& bounds) {
                                        columns->draw_drag_icon(renderer, bounds, theme.palette(),
                                                                icons);
                                    });
                                clipboard.start_drag(std::move(formats), window.surface(), icon,
                                                     seat.input_serial());
                            },
                        .cancel = [&] { clipboard.cancel_drag(); },
                        .types = [&] { return clipboard.drop_types(); },
                        .source_actions = [&] { return clipboard.drop_source_actions(); },
                        .action = [&] { return clipboard.drop_action(); },
                        .accept =
                            [&](const std::optional<std::string>& type, std::uint32_t actions,
                                std::uint32_t preferred) {
                                clipboard.accept_drop(type, actions, preferred);
                            },
                        .read =
                            [&](const std::string& type, views::DragAndDrop::Done done) {
                                clipboard.read_drop(type, std::move(done));
                            },
                        .finish = [&] { clipboard.finish_drop(); },
                        .modifiers = [&] { return seat.modifiers(); },
                    });
                }
                columns->set_panes_enabled(!picker_request);
                columns->apply(window_state);
                columns->set_sidebar_config(config.sidebar);
                const auto launch_location =
                    views::startup_location(config.startup, remembered.state, start_dir);
                if (picker_request) {
                    // An application-provided folder wins; otherwise use the picker's last folder.
                    const auto location = views::startup_location(
                        {.mode = core::Config::Startup::Mode::last}, remembered.state, start_dir);
                    columns->open(start_folder(location.folder));
                } else if (initial && reveal) {
                    columns->open(start_folder({}));
                    columns->show_items(requested_paths, reveal_details);
                } else if (initial && open_folders) {
                    columns->open(requested_paths.front());
                    if (requested_paths.size() > 1) {
                        columns->open_tabs({requested_paths.begin() + 1, requested_paths.end()});
                    }
                } else if (!initial) {
                    columns->open(folder);
                } else if (!launch_location.restore_tabs ||
                           !columns->restore_session(remembered.state)) {
                    columns->open(start_folder(launch_location.folder));
                }
                if (picker_request) {
                    columns->set_open_with_handlers({}, {});
                    columns->set_terminal_handler({});
                    columns->set_action_handoff({}, {});
                    picker = std::make_unique<views::FilePicker>(
                        *columns, *picker_request, mime, [&] { window.request_redraw(); },
                        [&](core::PickerResult result) {
                            picker_result = std::move(result);
                            loop.quit(0);
                        });
                    terminal_navigation = std::make_unique<platform::TerminalNavigation>(
                        loop,
                        [&](const std::filesystem::path& directory) {
                            picker->return_from_terminal(directory);
                        },
                        [&](std::string message, bool error) {
                            columns->show_message(std::move(message), error);
                        });
                    picker->set_terminal_navigation(
                        [&](const std::filesystem::path& directory) {
                            auto terminal_environment = platform::Environment::from_environment();
                            terminal_environment.configured_terminal = picker_terminal;
                            terminal_navigation->start(directory, terminal_environment);
                        },
                        [&] { terminal_navigation->cancel(); });
                }
            }
            startup.mark("browser");
            // The sidebar's places, kept current as things are mounted and bookmarked
            // (ROADMAP 5.9), with the disks udisks2 knows, mounted or not (ROADMAP 7.3).
            if (columns) {
                if (udisks->available()) {
                    columns->set_devices({
                        .mount =
                            [&](const std::string& volume, views::Devices::Mounted done) {
                                udisks->mount(volume, std::move(done));
                            },
                        .unmount =
                            [&](const std::string& volume, views::Devices::Done done) {
                                udisks->unmount(volume, std::move(done));
                            },
                        .eject =
                            [&](const std::string& volume, views::Devices::Done done) {
                                udisks->eject(volume, std::move(done));
                            },
                    });
                }
                // Remote bookmarks (smb://, sftp://, ftp://, WebDAV), mounted through gvfs on the
                // session bus.
                if (gvfs->available()) {
                    columns->set_remotes({
                        .mount =
                            [&](const std::string& uri, views::Remotes::Prompts prompts,
                                views::Remotes::Mounted done) {
                                gvfs->mount(uri, remote_prompts(std::move(prompts)),
                                            std::move(done));
                            },
                        .unmount =
                            [&](const std::string& uri, views::Remotes::Done done) {
                                gvfs->unmount(uri, std::move(done));
                            },
                    });
                }
                show_places();
                columns->set_places_reload([&places] { places->reload(); });
            }

            // rummage's own title bar where the compositor draws none (ROADMAP 7.4).
            views::TitleBar title_bar{
                {.move = [&] { window.move(display.seat(), seat.input_serial()); },
                 .resize =
                     [&](ui::Edge edge) {
                         window.resize(display.seat(), seat.input_serial(), edge);
                     },
                 .minimize = [&] { window.minimize(); },
                 .toggle_maximized = [&] { window.toggle_maximized(); },
                 .window_menu =
                     [&](float x, float y) {
                         window.show_window_menu(display.seat(), seat.input_serial(),
                                                 window.content_map().to_surface(x),
                                                 window.content_map().to_surface(y));
                     },
                 .close = [&] { window.close(); }},
                [&] { window.request_redraw(); },
                [&](ui::CursorShape shape) { seat.set_cursor(shape); }};
            bool bar_has_pointer =
                false;  // the content heard a leave when the bar took the pointer
            bool content_pressed = false;  // a press in the content keeps the pointer there

            std::string last_key = "Press a key (Ctrl+Q quits)";
            binding->key = [&](const ui::KeyEvent& event) {
                if (event.modifiers.ctrl &&
                    (event.keysym == XKB_KEY_q || event.keysym == XKB_KEY_Q)) {
                    log::info("quit requested");
                    quit_windows();
                    return;
                }
                if (picker) {
                    if (picker->editing_name() && event.modifiers.ctrl &&
                        (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V) &&
                        clipboard.available()) {
                        clipboard.read("text/plain;charset=utf-8",
                                       [&](std::optional<std::string> text) {
                                           if (text) {
                                               picker->paste(*text);
                                           }
                                       });
                    } else if (picker->editing_name() && event.modifiers.ctrl &&
                               (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C ||
                                event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X) &&
                               clipboard.available()) {
                        const auto text = picker->copy_name(event.keysym == XKB_KEY_x ||
                                                            event.keysym == XKB_KEY_X);
                        if (!text.empty()) {
                            clipboard.offer(
                                {{"text/plain;charset=utf-8", text}, {"text/plain", text}},
                                seat.input_serial());
                        }
                    } else {
                        picker->key(event);
                    }
                    return;
                }
                if (columns) {
                    columns->key(event, views::Browser::Clock::now());
                    return;
                }
                last_key = describe(event);
                log::debug("key: {}", last_key);
                window.request_redraw();
            };
            // Pointer test (ROADMAP 2.2): a hover/click target and the last pointer event.
            const ui::Rect target{760.0F, 32.0F, 160.0F, 48.0F};
            bool hovered = false;
            int clicks = 0;
            std::string last_pointer = "Move the pointer, click the button, scroll";
            binding->pointer = [&](const ui::PointerEvent& surface_event) {
                const auto event = window.content_map().to_content(surface_event);
                using Kind = ui::PointerEvent::Kind;
                if (window.client_decorations() && !content_pressed) {
                    if (title_bar.pointer(event)) {
                        if (!std::exchange(bar_has_pointer, true) && columns) {
                            columns->pointer({.kind = Kind::leave}, views::Browser::Clock::now());
                        }
                        return;
                    }
                    bar_has_pointer = false;
                }
                if (event.kind == Kind::press) {
                    content_pressed = true;
                } else if (event.kind == Kind::release || event.kind == Kind::leave) {
                    content_pressed = false;
                }
                if (picker) {
                    picker->pointer(event);
                    seat.set_cursor(picker->cursor_at(event.x, event.y));
                    return;
                }
                if (columns) {
                    columns->pointer(event, views::Browser::Clock::now());
                    return;
                }
                const bool inside = event.kind != Kind::leave && event.x >= target.x &&
                                    event.x < target.right() && event.y >= target.y &&
                                    event.y < target.bottom();
                if (inside != hovered) {
                    hovered = inside;
                    seat.set_cursor(inside ? ui::CursorShape::pointer : ui::CursorShape::standard);
                    window.request_redraw();
                }
                switch (event.kind) {
                    case Kind::press:
                        if (inside && event.button == ui::PointerButton::left) {
                            ++clicks;
                        }
                        last_pointer = std::format("press {} \u00d7{} at {:.1f}, {:.1f}",
                                                   button_name(event.button), event.click_count,
                                                   event.x, event.y);
                        break;
                    case Kind::scroll:
                        last_pointer =
                            std::format("scroll {:.2f}, {:.2f} px   wheel {:.2f}, {:.2f}{}{}",
                                        event.scroll_x, event.scroll_y, event.wheel_x,
                                        event.wheel_y, event.scroll_finger ? "   (touchpad)" : "",
                                        event.scroll_stop ? "   stop" : "");
                        break;
                    case Kind::leave:
                        last_pointer = "pointer left the window";
                        break;
                    default:
                        return;  // motion and enter only matter for hover
                }
                log::debug("pointer: {}", last_pointer);
                window.request_redraw();
            };
            binding->config = apply_config;
            binding->save = note_state;

            // Start timing: the first frame, then the first frame drawn with the folder loaded
            // (listed, its thumbnails made), each when the compositor shows it.
            bool first_drawn = false;
            bool first_shown = false;
            bool ready_drawn = false;
            bool ready_shown = false;

            // A view's destructor can request a synchronous redraw (the picker clears its
            // browser filter). Disconnect before anything captured by these handlers dies,
            // including on exception paths.
            struct FrameHandlers {
                ui::Window& window;
                core::EventLoop& loop;
                core::EventLoop::SourceId ambient_timer{0};
                core::EventLoop::SourceId close_timer{0};

                ~FrameHandlers() {
                    loop.remove(ambient_timer);
                    loop.remove(close_timer);
                    window.set_close_handler({});
                    window.set_frame_shown_handler({});
                    window.set_draw_handler({});
                }
            } frame_handlers{window, loop};

            window.set_close_handler([&] {
                if (columns && columns->file_operations_pending()) {
                    columns->show_message(
                        std::string{i18n::tr("Waiting for file operations before closing")}, false);
                    if (!frame_handlers.close_timer)
                        frame_handlers.close_timer = loop.add_timer(
                            std::chrono::milliseconds{100}, std::chrono::milliseconds{100}, [&] {
                                if (!columns->file_operations_pending())
                                    lifetime->close(loop, reap);
                            });
                    return;
                }
                lifetime->close(loop, reap);
            });
            if (initial && startup.active()) {
                window.set_frame_shown_handler([&] {
                    if (first_drawn && !first_shown) {
                        first_shown = true;
                        startup.mark("first-frame-shown");
                    }
                    if (ready_drawn && !ready_shown) {
                        ready_shown = true;
                        startup.mark("folder-shown");
                        if (startup.quit_when_shown()) {
                            loop.quit(0);
                        }
                    }
                });
            }
            window.set_draw_handler([&](ui::Renderer& renderer, const ui::Rect& bounds) {
                loop.remove(std::exchange(frame_handlers.ambient_timer, 0));
                if (columns)
                    columns->set_effects_active(window.state().activated);
                const bool initial_draw = initial && startup.active() && !first_drawn;
                const bool ready = !columns || !columns->loading();
                if (seat.pointer_surface() == window.surface())
                    seat.set_cursor_scale(
                        window.scale());  // the xcursor fallback's images follow it
                const theme::Palette& palette = theme.palette();
                ui::Rect content = bounds;
                if (window.client_decorations()) {
                    title_bar.set_state(window.state());
                    title_bar.draw(renderer, bounds, palette, icons, window.title());
                    content = title_bar.content(bounds);
                }
                if (startup.active()) {
                    if (!first_drawn) {
                        first_drawn = true;
                        startup.mark("first-draw");
                    }
                    ready_drawn = ready_drawn || ready;
                }
                if (!token.empty()) {
                    loop.post([&activation, weak = std::weak_ptr{binding},
                               value = std::exchange(token, {})] {
                        if (const auto owner = weak.lock(); owner && owner->window)
                            activation.activate(value, owner->window->surface());
                    });
                }
                if (columns) {
                    if (picker) {
                        picker->draw(renderer, content, palette, icons);
                    } else {
                        columns->draw(renderer, content, palette, icons,
                                      views::Browser::Clock::now());
                    }
                    renderer_for_stats = &renderer;
                    if (columns->animating()) {
                        window.request_redraw();  // next frame, paced by the compositor
                    } else if (const auto interval = columns->ambient_interval()) {
                        frame_handlers.ambient_timer =
                            loop.add_timer(*interval, std::chrono::milliseconds{0}, [&] {
                                frame_handlers.ambient_timer = 0;
                                window.request_redraw();
                            });
                    }
                    note_state();  // every change shows in a frame, so frames are where to look
                    if (initial_draw)
                        startup.mark("first-draw-finished");
                    return;
                }
                rummage::app::draw_test_scene(renderer, content, palette);
                renderer.draw_box(
                    target, {.fill = hovered ? palette.primary : palette.surface_container_high,
                             .radius = ui::Corners::all(10)});
                renderer.draw_text(std::format("Clicks: {}", clicks), target.x + 16.0F,
                                   target.y + 14.0F, 15.0F,
                                   hovered ? palette.on_primary : palette.on_surface);
                renderer.draw_text(last_pointer, 32.0F, bounds.bottom() - 56.0F, 14.0F,
                                   palette.secondary);
                renderer.draw_text(last_key, 32.0F, bounds.bottom() - 32.0F, 14.0F,
                                   palette.tertiary);
            });
            co_await *lifetime;
            note_state();
        };
        open_auxiliary = [&](std::shared_ptr<Binding> parent, std::filesystem::path path,
                             bool editable, bool drive, std::string label, bool settings,
                             std::string token) -> rummage::app::WindowSession {
            ui::Window window{
                display,
                egl,
                fonts,
                {.title = settings ? std::string{i18n::tr("Settings")}
                                   : i18n::trf("Properties — {}",
                                               label.empty() ? path.filename().string() : label),
                 .app_id = "rummage",
                 .width = settings ? 1080 : 760,
                 .height = 820,
                 .background = theme.palette().surface,
                 .force_client_decorations = client_decorations_forced(),
                 .background_opacity = config.appearance.background_opacity,
                 .parent_window = {}}};
            window.set_zoom(config.font.zoom());
            auto binding = std::make_shared<Binding>();
            binding->window = &window;
            binding->parent = parent;
            if (parent && parent->window)
                window.set_parent(parent->window);
            // Do not retain the origin's scope. Auxiliary windows can outlive it.
            parent.reset();
            bindings.push_back(binding);
            if (settings)
                settings_window = binding;
            Registration registration{[&] { remove_binding(binding); }};
            auto lifetime = std::make_shared<rummage::app::WindowLifetime>();
            views::PropertiesDialog properties{pool, mime, [&] { window.request_redraw(); }};
            views::SettingsDialog preferences{pool, [&] { window.request_redraw(); }};
            preferences.set_theme_status_handler([&] { return theme.status(); });
            preferences.set_system_reduced(portal->appearance().reduced_motion);
            properties.set_copy_handler([&](std::string text) {
                clipboard.offer({{"text/plain;charset=utf-8", text}, {"text/plain", text}},
                                seat.input_serial());
            });
            platform::Opener auxiliary_opener{
                loop,
                pool,
                mime,
                platform::Environment::from_environment(),
                [](std::string message, bool /*error*/) { log::warn("{}", message); },
                [&](std::function<void(std::string)> done) {
                    activation.request(display.seat(), seat.input_serial(), window.surface(),
                                       std::move(done));
                }};
            const auto changed = [&] {
                for (const auto& other : bindings)
                    if (other->browser)
                        other->browser->properties_changed(path);
            };
            if (settings)
                preferences.open(
                    config_path,
                    [&] {
                        auxiliary_opener.open(config_path.parent_path(),
                                              {{.name = config_path.filename().string(),
                                                .kind = fs::EntryKind::file}});
                    },
                    update_config);
            else
                properties.open(path, changed, editable, drive, label);
            struct Watches {
                fs::DirectoryWatcher& watcher;
                core::EventLoop& loop;
                fs::DirectoryWatcher::WatchId directory{};
                core::EventLoop::SourceId refresh{}, ambient{};
                ~Watches() {
                    watcher.unwatch(directory);
                    loop.remove(refresh);
                    loop.remove(ambient);
                }
            } watches{watcher, loop};
            if (!settings) {
                std::error_code error;
                watches.directory = watcher.watch(
                    path.parent_path(),
                    [&](const auto& events) {
                        if (std::ranges::none_of(events, [&](const auto& event) {
                                return event.name.empty() || event.name == path.filename();
                            }))
                            return;
                        loop.remove(watches.refresh);
                        watches.refresh = loop.add_timer(
                            std::chrono::milliseconds{100}, std::chrono::milliseconds{0}, [&] {
                                watches.refresh = 0;
                                if (!properties.saving() && !properties.dirty())
                                    properties.open(path, changed, editable, drive, label);
                            });
                    },
                    error);
            }
            views::TitleBar title_bar{
                {.move = [&] { window.move(display.seat(), seat.input_serial()); },
                 .resize =
                     [&](ui::Edge edge) {
                         window.resize(display.seat(), seat.input_serial(), edge);
                     },
                 .minimize = [&] { window.minimize(); },
                 .toggle_maximized = [&] { window.toggle_maximized(); },
                 .window_menu =
                     [&](float x, float y) {
                         window.show_window_menu(display.seat(), seat.input_serial(),
                                                 window.content_map().to_surface(x),
                                                 window.content_map().to_surface(y));
                     },
                 .close = [&] { window.close(); }},
                [&] { window.request_redraw(); },
                [&](ui::CursorShape shape) { seat.set_cursor(shape); }};
            bool content_pressed = false;
            bool closing = false;
            binding->key = [&, weak = std::weak_ptr{binding}](const ui::KeyEvent& event) {
                if (event.modifiers.ctrl &&
                    (event.keysym == XKB_KEY_q || event.keysym == XKB_KEY_Q)) {
                    quit_windows();
                    return;
                }
                if (event.modifiers.ctrl &&
                    (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V) &&
                    clipboard.available()) {
                    clipboard.read(
                        "text/plain;charset=utf-8", [&, weak](std::optional<std::string> text) {
                            if (const auto owner = weak.lock(); owner && owner->window && text) {
                                if (settings)
                                    preferences.paste(*text);
                                else
                                    properties.paste(*text);
                            }
                        });
                    return;
                }
                if (settings && event.modifiers.ctrl &&
                    (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C ||
                     event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X)) {
                    auto text = preferences.copy_selection(event.keysym == XKB_KEY_x ||
                                                           event.keysym == XKB_KEY_X);
                    if (!text.empty())
                        clipboard.offer({{"text/plain;charset=utf-8", text}, {"text/plain", text}},
                                        seat.input_serial());
                } else if (settings)
                    preferences.key(event);
                else
                    properties.key(event);
            };
            binding->pointer = [&](const ui::PointerEvent& surface_event) {
                const auto event = window.content_map().to_content(surface_event);
                if (window.client_decorations() && !content_pressed && title_bar.pointer(event))
                    return;
                if (event.kind == ui::PointerEvent::Kind::press)
                    content_pressed = true;
                else if (event.kind == ui::PointerEvent::Kind::release ||
                         event.kind == ui::PointerEvent::Kind::leave)
                    content_pressed = false;
                if (settings) {
                    preferences.pointer(event);
                    seat.set_cursor(preferences.cursor_at(event.x, event.y));
                } else {
                    properties.pointer(event);
                    seat.set_cursor(properties.cursor_at(event.x, event.y));
                }
            };
            binding->config = [&](const core::ConfigResult& fresh) {
                window.set_zoom(fresh.config.font.zoom());
                window.set_background_opacity(fresh.config.appearance.background_opacity);
                window.request_redraw();
            };
            // Disconnect while every captured local is still alive, including on exceptions.
            Registration handlers{[&] {
                window.set_draw_handler({});
                window.set_close_handler({});
            }};
            window.set_close_handler([&] {
                if (settings)
                    preferences.close();
                else {
                    closing = true;
                    properties.close();
                }
                if (settings ? !preferences.is_open() : !properties.is_open())
                    lifetime->close(loop, reap);
            });
            window.set_draw_handler([&](ui::Renderer& renderer, const ui::Rect& bounds) {
                loop.remove(std::exchange(watches.ambient, 0));
                if (!settings && closing && !properties.saving())
                    properties.close();
                if (settings ? !preferences.is_open() : !properties.is_open()) {
                    lifetime->close(loop, reap);
                    return;
                }
                if (!token.empty()) {
                    loop.post([&activation, weak = std::weak_ptr{binding},
                               value = std::exchange(token, {})] {
                        if (const auto owner = weak.lock(); owner && owner->window)
                            activation.activate(value, owner->window->surface());
                    });
                }
                auto content = bounds;
                if (window.client_decorations()) {
                    title_bar.set_state(window.state());
                    title_bar.draw(renderer, bounds, theme.palette(), icons, window.title());
                    content = title_bar.content(bounds);
                }
                if (settings) {
                    preferences.draw(renderer, content, theme.palette(), icons);
                    if (preferences.animating())
                        window.request_redraw();
                    else if (const auto interval = preferences.ambient_interval())
                        watches.ambient =
                            loop.add_timer(*interval, std::chrono::milliseconds{0}, [&] {
                                watches.ambient = 0;
                                window.request_redraw();
                            });
                } else
                    properties.draw(renderer, content, theme.palette(), icons);
            });
            co_await *lifetime;
        };
        std::unique_ptr<platform::BrowserInstance> instance;
        if (reuse) {
            try {
                instance = std::make_unique<platform::BrowserInstance>(
                    loop, instance_name, [&](platform::BrowserRequest request) {
                        auto target = last_browser.lock();
                        const bool created = !target || request.new_window;
                        if (created) {
                            sessions.push_back(open_browser(
                                request.show == platform::Show::folders
                                    ? std::filesystem::path{request.paths.front()}
                                    : std::filesystem::path{request.paths.front()}.parent_path(),
                                std::move(request.token), false));
                            reap();
                            target = last_browser.lock();
                            if (request.show == platform::Show::folders)
                                request.paths.erase(request.paths.begin());
                        }
                        if (!target || !target->browser)
                            return;
                        const std::vector<std::filesystem::path> paths{request.paths.begin(),
                                                                       request.paths.end()};
                        if (request.show == platform::Show::folders) {
                            const auto first = target->browser->active_tab() + 1;
                            target->browser->open_tabs(paths);
                            if (!created && !paths.empty())
                                target->browser->select_tab(first);
                        } else {
                            if (!created)
                                target->browser->new_tab();
                            target->browser->show_items(paths,
                                                        request.show == platform::Show::properties);
                        }
                        if (!request.token.empty())
                            activation.activate(request.token, target->window->surface());
                    });
            } catch (const std::exception& error) {
                // A simultaneous launch may have acquired the name while graphics started.
                const auto forwarded =
                    platform::forward_browser_request(instance_name, launch_request);
                if (forwarded != platform::BrowserForward::unavailable)
                    return forwarded == platform::BrowserForward::accepted ? 0 : 1;
                log::warn("window reuse unavailable: {}", error.what());
            }
        }
        sessions.push_back(open_browser({}, std::move(startup_token), true));
        reap();
        const int code = sessions.empty() ? 1 : loop.run();
        for (const auto& binding : bindings)
            if (binding->save)
                binding->save();
        sessions.clear();
        keeper.flush();
        if (picker_request) {
            if (picker_channel) {
                core::write_picker_result(4, picker_result);
                close(4);
            } else if (picker_result.response == 0) {
                for (const auto& path : picker_result.paths) {
                    std::fwrite(path.data(), 1, path.size(), stdout);
                    std::fputc('\0', stdout);
                }
                if (std::fflush(stdout) != 0) {
                    return 2;
                }
            }
            return code != 0 ? code
                             : (picker_channel ? 0 : static_cast<int>(picker_result.response));
        }
        return code;
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        return 1;
    }
}
