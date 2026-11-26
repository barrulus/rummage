#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
namespace {
std::filesystem::path tool(std::string_view name) {
    const char* path = std::getenv("PATH");
    const auto found = platform::find_program(name, path ? path : "");
    return found.value_or(std::filesystem::path{});
}
}  // namespace

bool Browser::start_terminal() {
    if (picker_mode_)
        return false;
    const auto folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash() || folder->is_recent() ||
        searching_results()) {
        show_message(std::string{i18n::tr("Terminal needs an ordinary local folder")}, true);
        return false;
    }
    auto& session = embedded_terminal_.session();
    if (!session.running()) {
        if (const auto error = session.start(folder->path()); !error.empty()) {
            show_message(error, true);
            return false;
        }
        terminal_browser_path_ = terminal_seen_path_ = folder->path();
        terminal_sync_target_.clear();
        terminal_result_.clear();
        terminal_result_reading_ = false;
        ++terminal_generation_;
    }
    embedded_terminal_.show(true);
    if (!terminal_timer_)
        terminal_timer_ =
            loop_.add_timer(std::chrono::milliseconds{100}, std::chrono::milliseconds{100},
                            [this] { terminal_tick(); });
    redraw_();
    return true;
}

void Browser::toggle_terminal() {
    if (embedded_terminal_.visible()) {
        embedded_terminal_.show(false);
        redraw_();
    } else
        start_terminal();
}

void Browser::focus_terminal() {
    if (!embedded_terminal_.visible())
        start_terminal();
    else {
        embedded_terminal_.focus(!embedded_terminal_.focused());
        redraw_();
    }
}

void Browser::terminal_tick() {
    if (!embedded_terminal_.visible())
        return;
    auto& session = embedded_terminal_.session();
    if (!session.running()) {
        terminal_result_.clear();
        terminal_script_.clear();
        return;
    }
    const auto folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash() || folder->is_recent() ||
        searching_results())
        return;
    const auto browser_path = folder->path();
    if (browser_path != terminal_browser_path_) {
        terminal_browser_path_ = browser_path;
        terminal_sync_target_ = browser_path;
        session.request_directory(browser_path);
    }
    const auto terminal_path = session.directory();
    if (!terminal_sync_target_.empty() && session.ready() && !session.directory_pending()) {
        terminal_sync_target_.clear();
        terminal_seen_path_ = terminal_path;
    }
    if (terminal_sync_target_.empty() && terminal_path != terminal_seen_path_ && session.ready()) {
        terminal_seen_path_ = terminal_path;
        // Browser navigation while the shell is busy takes precedence over an old shell cwd.
        if (terminal_path != browser_path && terminal_script_.empty()) {
            terminal_browser_path_ = terminal_path;
            open(terminal_path);
        }
    }
    if (!terminal_script_.empty() && !session.supports_sync()) {
        terminal_script_.clear();
        terminal_result_.clear();
        show_message(std::string{i18n::tr("Terminal workflows require Bash or zsh")}, true);
    }
    if (!terminal_script_.empty() && session.ready()) {
        if (session.command(terminal_script_))
            terminal_script_.clear();
    }
    if (!terminal_result_.empty() && !terminal_result_reading_ && session.ready() &&
        terminal_script_.empty()) {
        terminal_result_reading_ = true;
        const auto file = terminal_result_, root = terminal_result_root_,
                   session_path = session.session_directory();
        const auto generation = terminal_generation_;
        const bool zero = terminal_result_zero_;

        struct Result {
            std::filesystem::path path;
            bool directory{};
            std::string error;
        };

        pool_.submit(
            core::Priority::visible,
            [file, root, zero](const core::CancelToken&) {
                Result value;
                // Even an accidentally substituted FIFO or symlink must never block navigation.
                const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
                if (fd < 0)
                    return value;  // cancellation or a command which failed before writing
                struct stat info{};
                std::string result(8193, '\0');
                const bool regular = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
                const auto size = regular ? ::read(fd, result.data(), result.size()) : -1;
                ::close(fd);
                std::error_code ignored;
                std::filesystem::remove(file, ignored);
                if (size < 0 || size > 8192) {
                    value.error = std::string{i18n::tr("Invalid navigation result")};
                    return value;
                }
                result.resize(static_cast<std::size_t>(size));
                if (result.empty())
                    return value;
                if (zero) {
                    if (result.back() != '\0' || result.find('\0') != result.size() - 1) {
                        value.error = std::string{i18n::tr("Invalid navigation result")};
                        return value;
                    }
                    result.pop_back();
                } else {
                    if (result.back() == '\n')
                        result.pop_back();
                    if (result.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
                        value.error = std::string{i18n::tr("Invalid navigation result")};
                        return value;
                    }
                }
                if (result.empty())
                    return value;
                value.path = (root / result).lexically_normal();
                value.directory = std::filesystem::is_directory(value.path, ignored);
                return value;
            },
            [this, alive = alive_, generation, file, session_path](Result result) {
                if (!*alive || generation != terminal_generation_ || terminal_result_ != file ||
                    embedded_terminal_.session().session_directory() != session_path)
                    return;
                terminal_result_.clear();
                terminal_result_reading_ = false;
                if (!result.error.empty())
                    show_message(result.error, true);
                else if (!result.path.empty()) {
                    if (result.directory)
                        open(result.path);
                    else
                        show_items({result.path}, false);
                    embedded_terminal_.focus(false);
                }
            });
    }
}

void Browser::terminal_paste() {
    if (!system_.types || !system_.read)
        return;
    const auto types = system_.types();
    for (const std::string_view type : {"text/plain;charset=utf-8", "text/plain"}) {
        if (std::ranges::find(types, type) == types.end())
            continue;
        const auto session_path = embedded_terminal_.session().session_directory();
        system_.read(std::string{type},
                     [this, alive = alive_, session_path](const std::optional<std::string>& text) {
                         if (*alive && text && terminal_focused() &&
                             embedded_terminal_.session().session_directory() == session_path)
                             embedded_terminal_.session().paste(*text);
                     });
        break;
    }
}

bool Browser::terminal_workflow_available(int kind) {
    if (!platform::EmbeddedTerminal::available() || tool("sh").empty())
        return false;
    if (kind == 1)
        return !tool("fzf").empty() && !tool("find").empty();
    if (kind == 2)
        return !tool("zoxide").empty();
    return kind == 3;
}

void Browser::terminal_workflow(int kind) {
    const auto folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash() || folder->is_recent() ||
        searching_results() || picker_mode_) {
        show_message(std::string{i18n::tr("Terminal needs an ordinary local folder")}, true);
        return;
    }
    const auto sh = tool("sh");
    const auto program = kind == 1 ? tool("fzf") : kind == 2 ? tool("zoxide") : sh;
    if (program.empty() || sh.empty() || (kind == 1 && tool("find").empty())) {
        show_message(i18n::trf("Required command is not installed: {}", kind == 1   ? "fzf / find"
                                                                        : kind == 2 ? "zoxide"
                                                                                    : "sh"),
                     true);
        return;
    }
    const auto directory = folder->path();
    std::vector<std::filesystem::path> files;
    for (const auto& name : folder->selected_names())
        files.push_back(directory / name);
    const auto launch = [this, directory, files, program, sh,
                         kind](const std::string& command) -> std::string {
        if (command.size() > 8192 || command.find('\0') != std::string::npos)
            return std::string{i18n::tr("Command is too long")};
        if (!terminal_script_.empty() || !terminal_result_.empty())
            return std::string{i18n::tr("Wait for the current terminal workflow to finish")};
        if (embedded_terminal_.session().running() && !embedded_terminal_.session().supports_sync())
            return std::string{i18n::tr("Terminal workflows require Bash or zsh")};
        if (embedded_terminal_.session().running() && !embedded_terminal_.session().ready())
            return std::string{i18n::tr("Finish the terminal command or clear its input first")};
        if (!start_terminal())
            return std::string{i18n::tr("Cannot start terminal")};
        const auto quote = platform::shell_argument;
        std::string script = "cd -- " + quote(directory.string()) + " && ";
        if (kind == 1 || kind == 2) {
            terminal_result_ = embedded_terminal_.session().session_directory() /
                               ("navigation-result-" + std::to_string(++terminal_generation_));
            terminal_result_root_ = directory;
            terminal_result_zero_ = kind == 1;
            terminal_result_reading_ = false;
            std::error_code ignored;
            std::filesystem::remove(terminal_result_, ignored);
            if (kind == 1)
                script += quote(tool("find").string()) +
                          " . -maxdepth 64 \\( -type f -o -type d \\) -print0 | " +
                          quote(program.string()) + " --read0 --print0 --no-multi > " +
                          quote(terminal_result_.string());
            else
                script += quote(program.string()) + " query -- " + quote(command) + " > " +
                          quote(terminal_result_.string());
        } else {
            script += quote(sh.string()) + " -c " + quote(command) + " rummage";
            for (const auto& file : files) {
                script += " " + quote(file.string());
                if (script.size() > 59900)
                    return std::string{i18n::tr("Command is too long")};
            }
        }
        script += "; printf '\\n[exit %s]\\n' \"$?\"";
        if (script.size() > 60000) {
            terminal_result_.clear();
            return std::string{i18n::tr("Command is too long")};
        }
        terminal_script_ = std::move(script);
        return {};
    };
    if (kind == 1) {
        if (const auto error = launch(""); !error.empty())
            show_message(error, true);
    } else
        confirm_.open_name(
            kind == 2 ? std::string{i18n::tr("Jump with zoxide…")}
                      : std::string{i18n::tr("Run Shell Command…")},
            kind == 2
                ? std::string{i18n::tr("Directory query")}
                : std::string{i18n::tr("Current folder; selected paths are $1, $2, … or \"$@\"")},
            "", launch, std::string{i18n::tr("Run")});
}
}  // namespace rummage::views
