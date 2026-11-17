#include "rummage/platform/terminal_navigation.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/platform/exec.hpp"

namespace rummage::platform {
namespace {
constexpr std::size_t kMaxMessage = 8192;
volatile std::sig_atomic_t interrupted = 0;

void interrupt(int signal) { interrupted = signal; }

std::string quote(const std::string& text) {
    std::string out = "'";
    for (const char c : text)
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

sockaddr_un address(const std::filesystem::path& session) {
    sockaddr_un out{};
    out.sun_family = AF_UNIX;
    const auto path = (session / "handoff").string();
    if (path.size() < sizeof(out.sun_path))
        std::memcpy(out.sun_path, path.c_str(), path.size() + 1);
    return out;
}

bool write_script(const std::filesystem::path& path, const std::string& script) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const auto written = write(fd, script.data(), script.size());
    close(fd);
    return written == static_cast<ssize_t>(script.size());
}

std::string fish_quote(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\\' || c == '\'')
            out += '\\';
        out += c;
    }
    return out + "'";
}

bool prepare_shell(const std::filesystem::path& session, std::string_view shell) {
    if (shell == "fish") {
        // -C runs after the normal config, including conf.d snippets and autoload paths.
        // Fish normally dies by signal on HUP/TERM, which the supervisor rejects. A user's
        // signal handler can instead call exit: prefix those handlers with an abort marker
        // while retaining their definitions, other event bindings and captured variables.
        // Do not add our own signal observer: that would suppress Fish's default action.
        const auto aborted = fish_quote((session / "aborted").string());
        const std::string marker =
            "if builtin contains -- SIGHUP $argv; or builtin contains -- SIGTERM $argv; "
            "builtin true 2>/dev/null > " +
            aborted + "; end";
        return write_script(
            session / "init.fish",
            "set -l handlers (builtin functions --handlers --handlers-type signal)\n"
            "or return 1\n"
            "for handler in (string match --regex --groups-only "
            "'^(?:SIGHUP|SIGTERM) (.+)$' -- $handlers)\n"
            "    set -l definition (builtin functions --no-details -- $handler)\n"
            "    or return 1\n"
            "    builtin test (count $definition) -ge 2; or return 1\n"
            "    builtin eval (string join \\n -- $definition[1] " +
                fish_quote(marker) +
                " $definition[2..-1] | string collect)\n"
                "    or return 1\n"
                "end\n"
                // Only enable the directory handoff after every signal handler is protected.
                "function __rummage_directory_exit --on-event fish_exit\n"
                "    builtin pwd -P 2>/dev/null > " +
                fish_quote((session / "cwd").string()) +
                "\n"
                "end\n");
    }
    // Capture the physical directory, including embedded/trailing newlines. The supervisor
    // removes only pwd's terminating newline. Never interpolate a user's directory as code.
    const auto report =
        "__rummage_directory_exit() {\n"
        "  builtin pwd -P 2>/dev/null > " +
        quote((session / "cwd").string()) + " 2>/dev/null\n  return \"${1:-0}\"\n}\n";
    if (shell == "bash") {
        return write_script(
            session / "bashrc",
            "if [[ -r ~/.bashrc ]]; then builtin source ~/.bashrc; fi\n" + report +
                "__rummage_old_exit=$(builtin trap -p EXIT)\n"
                "if [[ -n $__rummage_old_exit ]]; then\n"
                "  __rummage_old_exit=${__rummage_old_exit#trap -- }\n"
                "  __rummage_old_exit=${__rummage_old_exit% EXIT}\n"
                "  builtin eval \"__rummage_old_exit=$__rummage_old_exit\"\n"
                "fi\n"
                "builtin trap -- 'if __rummage_directory_exit \"$?\"; then "
                "builtin eval \"$__rummage_old_exit\"; else "
                "builtin eval \"$__rummage_old_exit\"; fi' EXIT\n"
                "__rummage_old_hup=$(builtin trap -p HUP)\n"
                "if [[ -n $__rummage_old_hup ]]; then\n"
                "  __rummage_old_hup=${__rummage_old_hup#trap -- }\n"
                "  __rummage_old_hup=${__rummage_old_hup% SIGHUP}\n"
                "  builtin eval \"__rummage_old_hup=$__rummage_old_hup\"\n"
                "else\n"
                "  __rummage_old_hup='builtin trap - HUP; builtin kill -HUP \"$$\"'\n"
                "fi\n"
                "__rummage_directory_hangup() { : 2>/dev/null > " +
                quote((session / "aborted").string()) +
                "; }\n"
                "builtin trap -- '__rummage_directory_hangup; '"
                "\"$__rummage_old_hup\" HUP\n"
                "unset __rummage_old_hup\n");
    }
    // ZDOTDIR is redirected only while selecting our startup files. Restore it before
    // sourcing the user's files, honoring a new ZDOTDIR set by their .zshenv as well.
    const char* original = std::getenv("ZDOTDIR");  // NOLINT(concurrency-mt-unsafe)
    const std::string restore =
        original ? "export ZDOTDIR=" + quote(original) + "\n" : "unset ZDOTDIR\n";
    return write_script(session / ".zshenv", restore +
                                                 "if [[ -r ${ZDOTDIR-$HOME}/.zshenv ]]; then "
                                                 "source ${ZDOTDIR-$HOME}/.zshenv; fi\n"
                                                 "__rummage_zdot_set=${+ZDOTDIR}\n"
                                                 "__rummage_zdot=${ZDOTDIR-}\n"
                                                 "export ZDOTDIR=" +
                                                 quote(session.string()) + "\n") &&
           write_script(
               session / ".zshrc",
               "if (( __rummage_zdot_set )); then export ZDOTDIR=$__rummage_zdot; "
               "else unset ZDOTDIR; fi\n"
               "unset __rummage_zdot __rummage_zdot_set\n"
               "if [[ -r ${ZDOTDIR-$HOME}/.zshrc ]]; then source ${ZDOTDIR-$HOME}/.zshrc; fi\n" +
                   report + "zshexit_functions+=(__rummage_directory_exit)\n" +
                   // Zsh can run zshexit hooks and exit normally on HUP. Remember it explicitly,
                   // preserving both function-style and list-style user HUP traps.
                   "__rummage_hup_function=${functions[TRAPHUP]-}\n"
                   "__rummage_trap_function=${functions[trap]-}\n"
                   "builtin trap 2>/dev/null > " +
                   quote((session / "traps").string()) +
                   "\n"
                   "__rummage_traps=$(< " +
                   quote((session / "traps").string()) +
                   ")\n"
                   "__rummage_hup_command=\n"
                   "function trap {\n"
                   "  if [[ $1 == -- && $3 == HUP ]]; then __rummage_hup_command=$2; fi\n"
                   "}\n"
                   "builtin eval \"$__rummage_traps\"\n"
                   "unfunction trap\n"
                   "if [[ -n $__rummage_trap_function ]]; then "
                   "functions[trap]=$__rummage_trap_function; fi\n"
                   "unset __rummage_traps __rummage_trap_function\n"
                   "TRAPHUP() {\n"
                   "  : 2>/dev/null > " +
                   quote((session / "aborted").string()) +
                   "\n"
                   "  if [[ -n $__rummage_hup_function ]]; then\n"
                   "    builtin eval \"$__rummage_hup_function\"\n"
                   "  elif [[ -n $__rummage_hup_command ]]; then\n"
                   "    builtin eval \"$__rummage_hup_command\"\n"
                   "    return 0\n"
                   "  else\n"
                   "    return 1\n"
                   "  fi\n"
                   "}\n");
}

std::filesystem::path user_shell() {
    if (const char* shell = std::getenv("SHELL"); shell && *shell)  // NOLINT(concurrency-mt-unsafe)
        return shell;
    std::array<char, 16384> buffer{};
    passwd entry{};
    passwd* result = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 && result)
        return entry.pw_shell;
    return {};
}

void send_result(const std::filesystem::path& session, const std::string& result) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return;
    const auto target = address(session);
    if (target.sun_path[0])
        (void)sendto(fd, result.data(), result.size(), MSG_NOSIGNAL,
                     reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    close(fd);
}
}  // namespace

TerminalNavigation::TerminalNavigation(core::EventLoop& loop, Navigate navigate, Report report)
    : loop_{loop}, navigate_{std::move(navigate)}, report_{std::move(report)}, launcher_{loop} {}

TerminalNavigation::~TerminalNavigation() { cancel(); }

void TerminalNavigation::cancel() {
    if (source_)
        loop_.remove(source_);
    source_ = 0;
    if (socket_ >= 0)
        close(socket_);
    socket_ = -1;
    if (!session_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(session_, ignored);
        session_.clear();
    }
}

void TerminalNavigation::start(const std::filesystem::path& directory,
                               const Environment& environment, std::filesystem::path shell,
                               std::filesystem::path helper) {
    cancel();
    const auto fail = [&](const std::string& message) {
        cancel();
        report_(message, true);
    };
    if (shell.empty())
        shell = user_shell();
    const auto name = shell.filename();
    if (name != "bash" && name != "zsh" && name != "fish") {
        fail(std::string{
            i18n::tr("Navigate in terminal supports Bash, Zsh and Fish. Use the picker to "
                     "navigate with other shells.")});
        return;
    }
    const auto executable = find_program(shell.string(), environment.path);
    if (!executable) {
        fail(i18n::trf("Cannot find shell “{}”.", shell.string()));
        return;
    }
    if (!directory.is_absolute() || access(directory.c_str(), R_OK | X_OK) != 0) {
        fail(std::string{
            i18n::tr("Choose an accessible local folder before navigating in a terminal.")});
        return;
    }
    std::error_code error;
    if (helper.empty())
        helper = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || helper.empty()) {
        fail(std::string{i18n::tr("Cannot locate Rummage's terminal navigation helper.")});
        return;
    }
    std::string pattern = "/tmp/rummage-navigation-XXXXXX";
    if (!mkdtemp(pattern.data())) {
        fail(std::string{i18n::tr("Cannot create a private terminal navigation session.")});
        return;
    }
    session_ = pattern;
    socket_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const auto target = address(session_);
    if (socket_ < 0 ||
        bind(socket_, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0 ||
        !prepare_shell(session_, name.string())) {
        fail(std::string{i18n::tr("Cannot prepare the terminal navigation session.")});
        return;
    }
    const auto line = in_terminal({helper.string(), "--terminal-navigation-session",
                                   session_.string(), executable->string(), directory.string()},
                                  environment);
    if (!line) {
        fail(std::string{i18n::tr("No terminal found: set terminal.program in config.toml")});
        return;
    }
    source_ = loop_.add_fd(socket_, POLLIN, [this](short) { receive(); });
    const auto session = session_;
    error = launcher_.spawn(*line, directory, {}, [this, session](Launcher::Exit exit) {
        // A successful client may have handed off to a terminal server. Only failures are
        // actionable here; the helper reports the shell's completion independently.
        if (session_ == session && (exit.signal || exit.status)) {
            cancel();
            report_(
                std::string{i18n::tr(
                    "Terminal launch failed or the terminal closed without returning a folder.")},
                true);
        }
    });
    if (error) {
        fail(i18n::trf("Cannot launch terminal: {}", error.message()));
        return;
    }
    report_(std::string{i18n::tr("Navigate to your folder, then type exit to return here.")},
            false);
}

void TerminalNavigation::receive() {
    std::array<char, kMaxMessage> buffer{};
    const auto size = recv(socket_, buffer.data(), buffer.size(), MSG_TRUNC);
    if (size < 0)
        return;
    const std::string message{buffer.data(), static_cast<std::size_t>(std::min(
                                                 size, static_cast<ssize_t>(buffer.size())))};
    cancel();  // consume once, even for invalid or failed handoffs
    if (size >= static_cast<ssize_t>(buffer.size()) || message.empty() || message.front() != '/' ||
        message.find('\0') != std::string::npos) {
        report_(std::string{i18n::tr("Terminal navigation ended without a folder. You can keep "
                                     "navigating in the picker.")},
                true);
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(message, error) ||
        access(message.c_str(), R_OK | X_OK) != 0) {
        report_(i18n::trf("Cannot access returned folder “{}”. It may have been removed or its "
                          "permissions changed.",
                          message),
                true);
        return;
    }
    navigate_(message);
}

int run_terminal_navigation(const std::filesystem::path& session,
                            const std::filesystem::path& shell,
                            const std::filesystem::path& directory) {
    // This runs in a fresh process, before Rummage installs its signal mask or starts threads.
    // Supervision distinguishes normal exit (including `exit 7`) from HUP/crash/kill even if
    // a shell's EXIT trap happened to run during signal termination.
    struct stat info{};
    if (lstat(session.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 077) != 0)
        return 1;
    const bool bash = shell.filename() == "bash";
    const bool fish = shell.filename() == "fish";
    if (!bash && !fish && shell.filename() != "zsh")
        return 1;
    if (!directory.is_absolute() || chdir(directory.c_str()) != 0) {
        send_result(session, "failed");
        return 1;
    }
    struct sigaction action{};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGHUP, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    const pid_t child = fork();
    if (child == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        if (bash) {
            const auto rc = (session / "bashrc").string();
            execl(shell.c_str(), shell.c_str(), "--rcfile", rc.c_str(), "-i", nullptr);
        } else if (fish) {
            const auto init = "source " + fish_quote((session / "init.fish").string());
            execl(shell.c_str(), shell.c_str(), "-i", "--init-command", init.c_str(), nullptr);
        } else {
            setenv("ZDOTDIR", session.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
            execl(shell.c_str(), shell.c_str(), "-i", nullptr);
        }
        _exit(127);
    }
    if (child < 0) {
        send_result(session, "failed");
        return 1;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
        if (waited < 0 && errno == EINTR && interrupted)
            kill(child, interrupted);
    } while (waited < 0 && errno == EINTR);
    std::string result = "cancelled";
    if (waited == child && WIFEXITED(status) && !interrupted &&
        access((session / "aborted").c_str(), F_OK) != 0) {
        // Bounded and no symlink traversal, including when user startup hooks interfere.
        const int fd =
            open((session / "cwd").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0) {
            std::array<char, kMaxMessage> buffer{};
            const auto size = read(fd, buffer.data(), buffer.size());
            close(fd);
            if (size > 1 && size < static_cast<ssize_t>(buffer.size()) && buffer[0] == '/' &&
                buffer[static_cast<std::size_t>(size) - 1] == '\n')
                result.assign(buffer.data(), static_cast<std::size_t>(size) - 1);
        }
    }
    send_result(session, result);
    return 0;
}
}  // namespace rummage::platform
