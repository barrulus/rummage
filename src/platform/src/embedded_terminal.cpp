#include "rummage/platform/embedded_terminal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef RUMMAGE_TERMINAL
#  include <vterm.h>
#endif
#include "rummage/core/i18n.hpp"
#include "rummage/platform/exec.hpp"
extern char** environ;

namespace rummage::platform {
std::string shell_argument(std::string_view value) {
    std::string result = "'";
    for (const char ch : value)
        result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
}

namespace {
void reap_session(pid_t child, std::filesystem::path directory) {
    // The leader remains our unreaped child, so its session ID cannot be reused here.
    // pidfds pin each matching member across the check/signal race. Detached sessions
    // deliberately started by a shell command are outside this terminal's lifecycle.
    // Completed jobs are reclaimed on the next close; static futures join at process exit,
    // so the last window cannot exit before its HUP-ignoring jobs have been killed.
    static std::mutex mutex;
    static std::vector<std::future<void>> jobs;
    std::lock_guard lock{mutex};
    std::erase_if(jobs, [](auto& job) {
        return job.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    });
    jobs.push_back(std::async(std::launch::async, [child, directory = std::move(directory)] {
        if (child > 0) {
            if (DIR* proc = ::opendir("/proc")) {
                while (const auto* entry = ::readdir(proc)) {
                    char* end = nullptr;
                    const long number = std::strtol(entry->d_name, &end, 10);
                    if (!end || *end || number <= 0 || number > INT32_MAX)
                        continue;
                    const auto pid = static_cast<pid_t>(number);
                    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
                    if (fd < 0)
                        continue;
                    if (::getsid(pid) == child)
                        ::syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
                    ::close(fd);
                }
                ::closedir(proc);
            }
            ::kill(-child, SIGKILL);
            ::kill(child, SIGKILL);
            while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        std::error_code ignored;
        if (!directory.empty())
            std::filesystem::remove_all(directory, ignored);
    }));
}

[[maybe_unused]] std::string utf8(std::uint32_t c) {
    std::string out;
    if (c < 0x80)
        out += static_cast<char>(c);
    else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 63));
        out += static_cast<char>(0x80 | (c & 63));
    } else if (c <= 0x10ffff) {
        out += static_cast<char>(0xf0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 63));
        out += static_cast<char>(0x80 | ((c >> 6) & 63));
        out += static_cast<char>(0x80 | (c & 63));
    }
    return out;
}
}  // namespace

struct EmbeddedTerminal::Impl {
    core::EventLoop& loop;
    std::function<void()> changed;
    core::EventLoop::SourceId source{}, timer{};
    int master{-1};
    pid_t child{-1};
    int rows{24}, columns{80}, offset{};
    bool prompt{}, prompt_seen{}, draft{}, sync_supported{}, cursor_visible{true};
    bool probe_needed{}, probing{};
    std::uint64_t input_generation{}, probe_generation{}, prompt_generation{},
        probe_prompt_generation{};
    std::filesystem::path session, cwd, pending, syncing;
    std::string marker, probe_key, tail, outgoing, problem;
#ifdef RUMMAGE_TERMINAL
    VTerm* terminal{};
    VTermScreen* display{};
    std::deque<std::vector<VTermScreenCell>> history;

    static int push(int cols, const VTermScreenCell* cells, void* data) {
        auto& self = *static_cast<Impl*>(data);
        self.history.emplace_back(cells, cells + std::min(cols, 240));
        if (self.history.size() > 1000)
            self.history.pop_front();
        return 1;
    }

    static int pop(int cols, VTermScreenCell* cells, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (self.history.empty())
            return 0;
        auto row = std::move(self.history.back());
        self.history.pop_back();
        VTermScreenCell blank{};
        // libvterm advances through restored cells by width. Padding a narrow history
        // row with zero-width cells makes its resize loop stop making progress.
        blank.width = 1;
        blank.fg.type = VTERM_COLOR_DEFAULT_FG;
        blank.bg.type = VTERM_COLOR_DEFAULT_BG;
        std::fill(cells, cells + cols, blank);
        std::copy_n(row.begin(), std::min(row.size(), static_cast<std::size_t>(cols)), cells);
        return 1;
    }

    static int property(VTermProp prop, VTermValue* value, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (prop == VTERM_PROP_CURSORVISIBLE)
            self.cursor_visible = value->boolean != 0;
        return 1;  // titles, clipboard and URL side effects deliberately ignored
    }

    static void output(const char* bytes, size_t size, void* data) {
        auto& self = *static_cast<Impl*>(data);
        if (size <= 65536 - self.outgoing.size())
            self.outgoing.append(bytes, size);
        else
            self.problem = i18n::tr("Terminal input queue is full");
    }
#endif
    Impl(core::EventLoop& owner, std::function<void()> notify)
        : loop{owner}, changed{std::move(notify)} {
#ifdef RUMMAGE_TERMINAL
        terminal = vterm_new(rows, columns);
        vterm_set_utf8(terminal, 1);
        display = vterm_obtain_screen(terminal);
        static const auto callbacks = [] {
            VTermScreenCallbacks value{};
            value.sb_pushline = push;
            value.sb_popline = pop;
            value.settermprop = property;
            return value;
        }();
        vterm_screen_set_callbacks(display, &callbacks, this);
        vterm_screen_enable_altscreen(display, 1);
        vterm_output_set_callback(terminal, output, this);
        VTermColor fg{}, bg{};
        vterm_color_rgb(&fg, 216, 222, 233);
        vterm_color_rgb(&bg, 17, 19, 26);
        vterm_state_set_default_colors(vterm_obtain_state(terminal), &fg, &bg);
        vterm_screen_reset(display, 1);
#endif
    }

    ~Impl() {
        stop();
#ifdef RUMMAGE_TERMINAL
        vterm_free(terminal);
#endif
    }

    void stop() {
        loop.remove(source);
        loop.remove(timer);
        source = timer = 0;
        if (master >= 0) {
            ::close(master);
            master = -1;
        }
        if (child > 0 || !session.empty())
            reap_session(child, std::move(session));
        child = -1;
        session.clear();
        outgoing.clear();
        prompt = prompt_seen = false;
        draft = false;
        probe_needed = probing = false;
        input_generation = probe_generation = 0;
        prompt_generation = probe_prompt_generation = 0;
        pending.clear();
        syncing.clear();
        tail.clear();
    }

    bool ready() const {
        return child > 0 && master >= 0 && prompt && !draft && !probing && outgoing.empty() &&
               sync_supported && ::tcgetpgrp(master) == child;
    }

    void user_input() {
        ++input_generation;
        // A foreground program may consume this input, or leave it queued for the
        // shell. Ask its line editor after the next prompt; process ownership alone
        // cannot distinguish top's q from typeahead during sleep.
        if (prompt_seen && !prompt)
            probe_needed = true;
        draft = true;
    }

    void flush() {
        if (master < 0)
            return;
        while (!outgoing.empty()) {
            const auto n = ::write(master, outgoing.data(), outgoing.size());
            if (n > 0)
                outgoing.erase(0, static_cast<std::size_t>(n));
            else {
                if (n < 0 && errno != EAGAIN && errno != EINTR)
                    problem = i18n::tr("Terminal input failed");
                break;
            }
        }
    }

    void tick() {
        if (child <= 0)
            return;
        if (probe_needed && !probing && prompt && draft && sync_supported && outgoing.empty() &&
            ::tcgetpgrp(master) == child) {
            probing = true;
            probe_needed = false;
            probe_generation = input_generation;
            probe_prompt_generation = prompt_generation;
            outgoing += probe_key;
        }
        std::array<char, 8192> path{};
        const auto count = ::readlink(("/proc/" + std::to_string(child) + "/cwd").c_str(),
                                      path.data(), path.size());
        if (count > 0 && count < static_cast<ssize_t>(path.size())) {
            const std::filesystem::path next{
                std::string{path.data(), static_cast<std::size_t>(count)}};
            if (next != cwd) {
                cwd = next;
                changed();
            }
        }
        if (ready() && !syncing.empty()) {
            if (cwd != syncing)
                problem = i18n::tr("Terminal directory synchronization failed");
            syncing.clear();
        }
        if (ready() && !pending.empty()) {
            if (pending != cwd) {
                outgoing += " cd -- " + shell_argument(pending.string()) + "\n";
                syncing = pending;
                prompt = false;
            }
            pending.clear();
        }
        flush();
        // Never reap the session leader before cleanup: its ID protects member signalling.
    }

    void receive() {
#ifdef RUMMAGE_TERMINAL
        std::array<char, 16384> bytes{};
        const auto n = ::read(master, bytes.data(), bytes.size());
        if (n > 0) {
            const std::string_view data{bytes.data(), static_cast<std::size_t>(n)};
            tail.append(data);
            if (!marker.empty()) {
                while (true) {
                    const auto at = tail.find(marker);
                    if (at == std::string::npos)
                        break;
                    const auto end = tail.find('\a', at + marker.size());
                    if (end == std::string::npos)
                        break;
                    const auto status = tail.substr(at + marker.size(), end - at - marker.size());
                    if (status == "prompt") {
                        prompt = prompt_seen = true;
                        // The synchronous editor widget cannot outlive this prompt.
                        // Ctrl+C may have interrupted it before it acknowledged.
                        probing = false;
                        ++prompt_generation;
                    } else if (status == "continuation") {
                        prompt = false;
                        probing = false;
                        ++prompt_generation;
                    } else if (probing && (status == "clean" || status == "dirty")) {
                        probing = false;
                        // A delayed acknowledgement must never erase newer typing.
                        if (status == "clean" && probe_generation == input_generation && prompt &&
                            probe_prompt_generation == prompt_generation && outgoing.empty())
                            draft = false;
                    }
                    tail.erase(0, end + 1);
                }
                const auto keep = marker.size() + 16;
                if (tail.size() > keep)
                    tail.erase(0, tail.size() - keep);
            }
            vterm_input_write(terminal, data.data(), data.size());
            vterm_screen_flush_damage(display);
            flush();
            changed();
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            problem = i18n::tr("Terminal session ended");
            stop();
            changed();
        }
#endif
    }
};

EmbeddedTerminal::EmbeddedTerminal(core::EventLoop& loop, std::function<void()> changed)
    : impl_{std::make_unique<Impl>(loop, std::move(changed))} {}

EmbeddedTerminal::~EmbeddedTerminal() = default;

bool EmbeddedTerminal::available() {
#ifdef RUMMAGE_TERMINAL
    return true;
#else
    return false;
#endif
}

std::string EmbeddedTerminal::start(const std::filesystem::path& directory,
                                    std::filesystem::path shell, std::filesystem::path helper) {
    auto& p = *impl_;
    p.stop();
    p.problem.clear();
#ifdef RUMMAGE_TERMINAL
    p.history.clear();
    p.offset = 0;
    vterm_screen_reset(p.display, 1);
#endif
    if (!available())
        return std::string{i18n::tr("Embedded terminal support is not installed")};
    if (!directory.is_absolute())
        return std::string{i18n::tr("Terminal needs an ordinary local folder")};
    if (shell.empty()) {
        const char* value = std::getenv("SHELL");
        shell = value ? value : "/bin/sh";
    }
    if (!shell.is_absolute())
        return std::string{i18n::tr("Shell must be an absolute executable path")};
    if (helper.empty()) {
        std::array<char, 8192> executable{};
        const auto n = ::readlink("/proc/self/exe", executable.data(), executable.size());
        if (n > 0) {
            const auto bin =
                std::filesystem::path{std::string{executable.data(), static_cast<std::size_t>(n)}}
                    .parent_path();
            for (const auto& candidate :
                 {bin / "rummage-terminal", bin / "../libexec/rummage/rummage-terminal",
                  bin.parent_path() / "rummage-terminal"})
                if (::access(candidate.c_str(), X_OK) == 0) {
                    helper = candidate;
                    break;
                }
        }
    }
    if (helper.empty())
        return std::string{i18n::tr("Embedded terminal helper is not installed")};
    std::array<char, 64> name{};
    const std::string pattern = "/tmp/rummage-terminal-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), name.begin());
    if (!::mkdtemp(name.data()))
        return std::string{i18n::tr("Cannot create private terminal session")};
    p.session = name.data();
    p.marker = "\033]777;" + p.session.filename().string() + ";";
    p.probe_key = "\033[777;" + p.session.filename().string() + "~";
    const std::string signal = "printf '\\033]777;" + p.session.filename().string();
    const std::string prompt_signal = signal + ";prompt\\007'";
    const std::string probe_signal = signal + ";%s\\007' \"$state\"";
    const auto continuation_prefix =
        shell_argument("\\[\\e]777;" + p.session.filename().string() + ";continuation\\a\\]");
    std::ofstream(p.session / "bashrc")
        // Readline's buffer can be empty at PS2 while Bash retains an unfinished
        // command. Refresh this guard after user prompt hooks; a readonly PS2
        // which cannot accept it must never advertise a synchronization prompt.
        << "if [[ -f ~/.bashrc ]]; then source ~/.bashrc; fi\n__rummage_prompt() { local prefix="
        << continuation_prefix
        << "; if [[ $PS2 != \"$prefix\"* ]]; then PS2=\"$prefix${PS2-> }\" || return; fi; "
        << prompt_signal << "; }\nPROMPT_COMMAND+=(__rummage_prompt)\n"
        << "__rummage_probe() { local state=dirty; [[ -z $READLINE_LINE ]] && state=clean; "
        << probe_signal << "; }\n"
        << "for __rummage_map in emacs-standard vi-insert vi-command; do bind -m "
           "\"$__rummage_map\" -x "
        << shell_argument("\"\\e[777;" + p.session.filename().string() + "~\":__rummage_probe")
        << "; done\nunset __rummage_map\n";
    std::ofstream(p.session / ".zshenv")
        << "ZDOTDIR=$RUMMAGE_ORIGINAL_ZDOTDIR\n"
        << "if [[ -f $RUMMAGE_ORIGINAL_ZDOTDIR/.zshenv ]]; then source "
           "\"$RUMMAGE_ORIGINAL_ZDOTDIR/.zshenv\"; fi\nRUMMAGE_ORIGINAL_ZDOTDIR=$ZDOTDIR\nZDOTDIR="
        << shell_argument(p.session.string()) << "\n";
    std::ofstream(p.session / ".zshrc")
        << "ZDOTDIR=$RUMMAGE_ORIGINAL_ZDOTDIR\nif [[ -f $ZDOTDIR/.zshrc ]]; then source "
           "\"$ZDOTDIR/.zshrc\"; fi\n__rummage_prompt() { "
        << prompt_signal << "; }\nautoload -Uz add-zsh-hook\nadd-zsh-hook precmd __rummage_prompt\n"
        << "__rummage_probe() { local state=dirty; [[ $CONTEXT == start && -z $BUFFER && "
           "-z $PREBUFFER && $PENDING == 0 && $KEYS_QUEUED_COUNT == 0 ]] && state=clean; "
        << probe_signal << "; }\nzle -N __rummage_probe\n"
        << "for __rummage_map in emacs viins vicmd main; do bindkey -M \"$__rummage_map\" "
        << shell_argument(p.probe_key) << " __rummage_probe; done\nunset __rummage_map\n";
    p.sync_supported = shell.filename() == "bash" || shell.filename() == "zsh";
    p.master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (p.master < 0 || ::grantpt(p.master) < 0 || ::unlockpt(p.master) < 0) {
        p.stop();
        return std::string{i18n::tr("Cannot open terminal PTY")};
    }
    std::array<char, 256> slave_name{};
    if (::ptsname_r(p.master, slave_name.data(), slave_name.size()) != 0) {
        p.stop();
        return std::string{i18n::tr("Cannot name terminal PTY")};
    }
    const int slave = ::open(slave_name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        p.stop();
        return std::string{i18n::tr("Cannot open terminal slave")};
    }
    winsize size{.ws_row = static_cast<unsigned short>(p.rows),
                 .ws_col = static_cast<unsigned short>(p.columns),
                 .ws_xpixel = 0,
                 .ws_ypixel = 0};
    ::ioctl(slave, TIOCSWINSZ, &size);
    std::array<std::string, 4> args{helper.string(), directory.string(), shell.string(),
                                    p.session.string()};
    std::array<char*, 5> argv{args[0].data(), args[1].data(), args[2].data(), args[3].data(),
                              nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd < 3; ++fd)
        posix_spawn_file_actions_adddup2(&actions, slave, fd);
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    posix_spawnattr_t attrs;
    posix_spawnattr_init(&attrs);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attrs, &empty);
    posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETSIGMASK);
    const int result = ::posix_spawn(&p.child, argv[0], &actions, &attrs, argv.data(), environ);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    ::close(slave);
    if (result) {
        p.child = -1;
        p.stop();
        return std::error_code{result, std::generic_category()}.message();
    }
    p.cwd = directory;
    p.pending.clear();
    p.source = p.loop.add_fd(p.master, POLLIN, [&p](short) { p.receive(); });
    p.timer = p.loop.add_timer(std::chrono::milliseconds{100}, std::chrono::milliseconds{100},
                               [&p] { p.tick(); });
    return {};
}

void EmbeddedTerminal::close() { impl_->stop(); }

void EmbeddedTerminal::resize(int rows, int columns) {
    auto& p = *impl_;
    rows = std::clamp(rows, 2, 100);
    columns = std::clamp(columns, 10, 240);
    if (rows == p.rows && columns == p.columns)
        return;
    p.rows = rows;
    p.columns = columns;
#ifdef RUMMAGE_TERMINAL
    vterm_set_size(p.terminal, rows, columns);
    vterm_screen_flush_damage(p.display);
#endif
    if (p.master >= 0) {
        winsize size{.ws_row = static_cast<unsigned short>(rows),
                     .ws_col = static_cast<unsigned short>(columns),
                     .ws_xpixel = 0,
                     .ws_ypixel = 0};
        ::ioctl(p.master, TIOCSWINSZ, &size);
    }
}

void EmbeddedTerminal::key(Key key, std::string_view text, bool ctrl, bool alt, bool shift) {
#ifdef RUMMAGE_TERMINAL
    auto& p = *impl_;
    if (p.master < 0)
        return;
    p.offset = 0;
    const bool interrupted_probe = ctrl && text == "c" && p.probing;
    p.user_input();
    if (interrupted_probe) {
        // Ctrl+C can interrupt a line-editor widget's child without a new prompt hook.
        // Keep the known prompt, but require a fresh editor acknowledgement after the
        // outstanding widget returns. Its old response cannot clear newer input.
        p.probe_needed = true;
    } else if (key == Key::enter || (ctrl && (text == "c" || text == "d"))) {
        p.prompt = false;
        p.draft = !p.prompt_seen;
    }
    const auto modifiers = static_cast<VTermModifier>(
        (ctrl ? VTERM_MOD_CTRL : 0) | (alt ? VTERM_MOD_ALT : 0) | (shift ? VTERM_MOD_SHIFT : 0));
    constexpr std::array<int, 27> keys{
        VTERM_KEY_NONE,         VTERM_KEY_ENTER,        VTERM_KEY_TAB,
        VTERM_KEY_BACKSPACE,    VTERM_KEY_ESCAPE,       VTERM_KEY_UP,
        VTERM_KEY_DOWN,         VTERM_KEY_LEFT,         VTERM_KEY_RIGHT,
        VTERM_KEY_HOME,         VTERM_KEY_END,          VTERM_KEY_INS,
        VTERM_KEY_DEL,          VTERM_KEY_PAGEUP,       VTERM_KEY_PAGEDOWN,
        VTERM_KEY_FUNCTION(1),  VTERM_KEY_FUNCTION(2),  VTERM_KEY_FUNCTION(3),
        VTERM_KEY_FUNCTION(4),  VTERM_KEY_FUNCTION(5),  VTERM_KEY_FUNCTION(6),
        VTERM_KEY_FUNCTION(7),  VTERM_KEY_FUNCTION(8),  VTERM_KEY_FUNCTION(9),
        VTERM_KEY_FUNCTION(10), VTERM_KEY_FUNCTION(11), VTERM_KEY_FUNCTION(12)};
    if (key != Key::none)
        vterm_keyboard_key(
            p.terminal, static_cast<VTermKey>(keys.at(static_cast<std::size_t>(key))), modifiers);
    else
        for (std::size_t i = 0; i < text.size();) {
            const auto first = static_cast<unsigned char>(text[i++]);
            std::uint32_t value = first;
            unsigned more = 0;
            if (first >= 0xf0) {
                value = first & 7;
                more = 3;
            } else if (first >= 0xe0) {
                value = first & 15;
                more = 2;
            } else if (first >= 0xc0) {
                value = first & 31;
                more = 1;
            }
            while (more-- && i < text.size())
                value = (value << 6) | (static_cast<unsigned char>(text[i++]) & 63);
            vterm_keyboard_unichar(p.terminal, value, modifiers);
        }
    p.flush();
    p.changed();
#else
    (void)key;
    (void)text;
    (void)ctrl;
    (void)alt;
    (void)shift;
#endif
}

void EmbeddedTerminal::paste(std::string_view text) {
#ifdef RUMMAGE_TERMINAL
    if (text.size() > 65500 || text.size() + 32 > 65536 - impl_->outgoing.size()) {
        impl_->problem = i18n::tr("Terminal paste exceeds 64 KiB");
        return;
    }
    auto& p = *impl_;
    p.user_input();
    vterm_keyboard_start_paste(p.terminal);
    Impl::output(text.data(), text.size(), &p);
    vterm_keyboard_end_paste(p.terminal);
    p.flush();
#else
    (void)text;
#endif
}

void EmbeddedTerminal::scroll(int lines) {
#ifdef RUMMAGE_TERMINAL
    impl_->offset = std::clamp(impl_->offset + lines, 0, static_cast<int>(impl_->history.size()));
    impl_->changed();
#else
    (void)lines;
#endif
}

EmbeddedTerminal::Screen EmbeddedTerminal::screen() const {
    const auto& p = *impl_;
    Screen result{.rows = p.rows,
                  .columns = p.columns,
                  .cursor_row = 0,
                  .cursor_column = 0,
                  .cursor_visible = false,
                  .cells = {}};
#ifdef RUMMAGE_TERMINAL
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(p.terminal), &cursor);
    result.cursor_row = cursor.row;
    result.cursor_column = cursor.col;
    result.cursor_visible = p.cursor_visible && p.offset == 0;
    result.cells.reserve(static_cast<std::size_t>(p.rows * p.columns));
    for (int row = 0; row < p.rows; ++row)
        for (int col = 0; col < p.columns; ++col) {
            VTermScreenCell cell{};
            cell.fg.type = VTERM_COLOR_DEFAULT_FG;
            cell.bg.type = VTERM_COLOR_DEFAULT_BG;
            const int source = row - p.offset;
            if (source < 0) {
                const int h = static_cast<int>(p.history.size()) + source;
                if (h >= 0 && col < static_cast<int>(p.history[static_cast<std::size_t>(h)].size()))
                    cell = p.history[static_cast<std::size_t>(h)][static_cast<std::size_t>(col)];
            } else
                vterm_screen_get_cell(p.display, {source, col}, &cell);
            Cell out;
            out.width = cell.chars[0] == UINT32_MAX ? 0 : cell.width;
            if (!cell.attrs.conceal)
                for (const auto c : cell.chars)
                    if (c && c != UINT32_MAX)
                        out.text += utf8(c);
            const auto role = [](const VTermColor& color) {
                if (VTERM_COLOR_IS_DEFAULT_FG(&color))
                    return ColorRole::foreground;
                if (VTERM_COLOR_IS_DEFAULT_BG(&color))
                    return ColorRole::background;
                return ColorRole::explicit_rgb;
            };
            out.fg_role = role(cell.fg);
            out.bg_role = role(cell.bg);
            auto fg = cell.fg, bg = cell.bg;
            vterm_screen_convert_color_to_rgb(p.display, &fg);
            vterm_screen_convert_color_to_rgb(p.display, &bg);
            const auto rgb = [](const VTermColor& c) {
                return (static_cast<std::uint32_t>(c.rgb.red) << 16) |
                       (static_cast<std::uint32_t>(c.rgb.green) << 8) | c.rgb.blue;
            };
            out.fg = rgb(fg);
            out.bg = rgb(bg);
            if (cell.attrs.reverse) {
                std::swap(out.fg, out.bg);
                std::swap(out.fg_role, out.bg_role);
            }
            out.underline = cell.attrs.underline != 0;
            result.cells.push_back(std::move(out));
        }
#endif
    return result;
}

std::string EmbeddedTerminal::text() const {
    const auto value = screen();
    std::string out;
    for (std::size_t i = 0; i < value.cells.size(); ++i) {
        if (value.cells[i].width != 0)
            out += value.cells[i].text.empty() ? " " : value.cells[i].text;
        if ((i + 1) % static_cast<std::size_t>(value.columns) == 0)
            out += '\n';
    }
    return out;
}

bool EmbeddedTerminal::running() const { return impl_->child > 0; }

bool EmbeddedTerminal::ready() const { return impl_->ready(); }

bool EmbeddedTerminal::directory_pending() const {
    return !impl_->pending.empty() || !impl_->syncing.empty();
}

bool EmbeddedTerminal::supports_sync() const { return impl_->sync_supported; }

std::filesystem::path EmbeddedTerminal::directory() const { return impl_->cwd; }

void EmbeddedTerminal::request_directory(const std::filesystem::path& path) {
    if (path.is_absolute() && path.string().size() <= 8192 &&
        path.string().find('\0') == std::string::npos)
        impl_->pending = path;
}

bool EmbeddedTerminal::command(std::string_view script) {
    auto& p = *impl_;
    if (!p.ready() || script.size() > 60000 || script.find('\0') != std::string_view::npos)
        return false;
    p.pending.clear();
    p.outgoing += ' ';
    p.outgoing += script;
    p.outgoing += '\n';
    p.prompt = false;
    p.draft = false;
    p.flush();
    return true;
}

std::filesystem::path EmbeddedTerminal::session_directory() const { return impl_->session; }

const std::string& EmbeddedTerminal::error() const { return impl_->problem; }
}  // namespace rummage::platform
