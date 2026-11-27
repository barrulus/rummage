#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <thread>
#include <unistd.h>

#include <doctest/doctest.h>

#include "rummage/platform/embedded_terminal.hpp"
#include "temp_dir.hpp"
using rummage::platform::EmbeddedTerminal;
using namespace std::chrono_literals;

namespace {
bool until(rummage::core::EventLoop& loop, const std::function<bool()>& done,
           std::chrono::milliseconds limit = 3000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        loop.add_timer(10ms, 0ms, [&] { loop.quit(); });
        loop.run();
    }
    return done();
}

void wait(rummage::core::EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&] { loop.quit(); });
    loop.run();
}

struct Fixture {
    rummage::test::TempDir temp;
    rummage::core::EventLoop loop;
    EmbeddedTerminal terminal{loop, [] {}};

    Fixture() {
        ::setenv("HOME", temp.path.c_str(), 1);
        ::unsetenv("ZDOTDIR");
    }

    void start(const std::filesystem::path& shell = RUMMAGE_TEST_BASH) {
        REQUIRE(terminal.start(temp.path, shell, RUMMAGE_TERMINAL_HELPER).empty());
        REQUIRE(until(loop, [&] { return terminal.ready(); }));
    }
};
}  // namespace

TEST_CASE("terminal: real PTY UTF8 colour resize shell quoting and bounded scrollback") {
    Fixture f;
    f.start();
    f.terminal.resize(12, 50);
    REQUIRE(f.terminal.command("printf '\\033[31mhello 世界\\033[0m\\n'; stty size"));
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && f.terminal.text().find("12 50") != std::string::npos;
    }));
    CHECK(f.terminal.text().find("hello 世界") != std::string::npos);
    const auto screen = f.terminal.screen();
    CHECK(screen.rows == 12);
    CHECK(screen.columns == 50);
    CHECK(std::ranges::any_of(screen.cells,
                              [](const auto& c) { return c.text == "h" && c.fg != 0xd8dee9; }));
    const auto strange = f.temp.path / "space ' $(touch SHOULD_NOT_EXIST) é\nfolder";
    std::filesystem::create_directory(strange);
    f.terminal.request_directory(strange);
    REQUIRE(until(f.loop, [&] { return f.terminal.directory() == strange && f.terminal.ready(); }));
    CHECK_FALSE(std::filesystem::exists(f.temp.path / "SHOULD_NOT_EXIST"));
    REQUIRE(f.terminal.command("printf '%s\\n' {1..2500}"));
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && f.terminal.text().find("2500") != std::string::npos;
    }));
    f.terminal.scroll(1000000);
    CHECK(f.terminal.screen().cells.size() == 600);
}

TEST_CASE("terminal: widening then growing restores narrow scrollback without hanging") {
    Fixture f;
    f.start();
    f.terminal.resize(8, 20);
    REQUIRE(f.terminal.command("printf 'row %03d\\n' {1..60}"));
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && f.terminal.text().find("row 060") != std::string::npos;
    }));
    bool alternate = false;
    SUBCASE("shell screen") {}
    SUBCASE("interactive alternate screen") {
        alternate = true;
        REQUIRE(
            f.terminal.command("printf '\\033[?1049hALT_READY'; IFS= read -r -n1 key; "
                               "printf '\\033[?1049l'"));
        REQUIRE(until(f.loop,
                      [&] { return f.terminal.text().find("ALT_READY") != std::string::npos; }));
    }
    f.terminal.resize(8, 80);
    // The old callback spins inside libvterm, before the event loop can run a watchdog.
    // Bound this synchronous regression so a reintroduced hang fails the test process.
    ::alarm(5);
    f.terminal.resize(20, 80);
    ::alarm(0);
    const auto screen = f.terminal.screen();
    CHECK(screen.rows == 20);
    CHECK(screen.columns == 80);
    CHECK(screen.cells.size() == 1600);
    if (alternate) {
        f.terminal.key(EmbeddedTerminal::Key::none, "q");
        REQUIRE(until(f.loop, [&] { return f.terminal.ready(); }));
    }
    REQUIRE(f.terminal.command("printf 'RESIZE_OK\\n'; stty size"));
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && f.terminal.text().find("RESIZE_OK") != std::string::npos &&
               f.terminal.text().find("20 80") != std::string::npos;
    }));
    CHECK(f.terminal.error().empty());
}

TEST_CASE("terminal: browser sync never overwrites a draft or foreground command") {
    Fixture f;
    f.start();
    const auto target = f.temp.path / "next";
    std::filesystem::create_directory(target);
    f.terminal.key(EmbeddedTerminal::Key::none, "echo DRAFT");
    f.terminal.request_directory(target);
    wait(f.loop, 180ms);
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK_FALSE(f.terminal.ready());
    f.terminal.key(EmbeddedTerminal::Key::none, "u", true);
    f.terminal.key(EmbeddedTerminal::Key::enter, "");
    REQUIRE(until(f.loop, [&] { return f.terminal.directory() == target && f.terminal.ready(); }));
    REQUIRE(f.terminal.command("sleep 0.4"));
    f.terminal.request_directory(f.temp.path);
    wait(f.loop, 150ms);
    CHECK(f.terminal.directory() == target);
    REQUIRE(
        until(f.loop, [&] { return f.terminal.directory() == f.temp.path && f.terminal.ready(); }));
}

TEST_CASE("terminal: teardown reaps HUP-ignoring foreground processes promptly") {
    Fixture f;
    f.start();
    const auto pidfile = f.temp.path / "pid";
    REQUIRE(f.terminal.command("printf '%s' \"$$\" > " +
                               rummage::platform::shell_argument(pidfile.string()) +
                               "; trap '' HUP; sleep 60"));
    REQUIRE(until(f.loop, [&] { return std::filesystem::exists(pidfile); }));
    pid_t child{};
    std::ifstream(pidfile) >> child;
    REQUIRE(child > 0);
    const auto begin = std::chrono::steady_clock::now();
    f.terminal.close();
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    REQUIRE(until(f.loop, [&] { return ::kill(child, 0) < 0 && errno == ESRCH; }));
    CHECK_FALSE(f.terminal.running());
}

TEST_CASE("terminal: quitting an interactive foreground job resumes queued directory tracking") {
    Fixture f;
    auto shell = std::filesystem::path{RUMMAGE_TEST_BASH};
    bool paste = false;
    SUBCASE("bash keyboard") {}
    SUBCASE("bash paste") { paste = true; }
    SUBCASE("bash vi mode") { f.temp.write(".bashrc", "set -o vi\n"); }
#ifdef RUMMAGE_TEST_ZSH
    SUBCASE("zsh keyboard") { shell = RUMMAGE_TEST_ZSH; }
    SUBCASE("zsh paste") {
        shell = RUMMAGE_TEST_ZSH;
        paste = true;
    }
    SUBCASE("zsh vi mode") {
        shell = RUMMAGE_TEST_ZSH;
        f.temp.write(".zshrc", "bindkey -v\n");
    }
#endif
    f.start(shell);
    const auto first = f.temp.path / "first", latest = f.temp.path / "latest";
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(latest);
    // A separate foreground process reads one key without Enter, like top's q.
    // Alternate-screen output also exercises returning to the shell's display.
    REQUIRE(f.terminal.command(rummage::platform::shell_argument(RUMMAGE_TEST_BASH) + " -c " +
                               rummage::platform::shell_argument(
                                   "printf '\\033[?1049hJOB_READY'; IFS= read -r -n1 key; "
                                   "printf '%s' \"$key\" > job-key; printf '\\033[?1049l'")));
    REQUIRE(
        until(f.loop, [&] { return f.terminal.text().find("JOB_READY") != std::string::npos; }));
    f.terminal.request_directory(first);
    wait(f.loop, 150ms);
    f.terminal.request_directory(latest);
    wait(f.loop, 150ms);
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK_FALSE(f.terminal.ready());
    // Both input paths must distinguish job input from a draft at the shell prompt.
    if (paste)
        f.terminal.paste("q");
    else
        f.terminal.key(EmbeddedTerminal::Key::none, "q");
    const bool resumed = until(f.loop, [&] {
        return f.terminal.ready() && !f.terminal.directory_pending() &&
               f.terminal.directory() == latest;
    });
    INFO(f.terminal.text());
    REQUIRE(resumed);
    std::string key;
    std::ifstream(f.temp.path / "job-key") >> key;
    CHECK(key == "q");
    f.terminal.request_directory(first);
    REQUIRE(until(f.loop, [&] { return f.terminal.ready() && f.terminal.directory() == first; }));
    REQUIRE(f.terminal.command("cd -- " + rummage::platform::shell_argument(f.temp.path.string())));
    REQUIRE(
        until(f.loop, [&] { return f.terminal.ready() && f.terminal.directory() == f.temp.path; }));
}

TEST_CASE("terminal: foreground typeahead stays protected at normal and continuation prompts") {
    Fixture f;
    auto shell = std::filesystem::path{RUMMAGE_TEST_BASH};
    bool continuation = false;
    SUBCASE("bash draft") {}
    SUBCASE("bash continuation") { continuation = true; }
    SUBCASE("bash prompt hook replaces PS2") {
        continuation = true;
        f.temp.write(".bashrc", "PROMPT_COMMAND=('PS2=\"more> \"')\n");
    }
#ifdef RUMMAGE_TEST_ZSH
    SUBCASE("zsh draft") { shell = RUMMAGE_TEST_ZSH; }
    SUBCASE("zsh continuation") {
        shell = RUMMAGE_TEST_ZSH;
        continuation = true;
    }
#endif
    f.start(shell);
    const auto target = f.temp.path / "next";
    std::filesystem::create_directory(target);
    REQUIRE(f.terminal.command(
        rummage::platform::shell_argument(RUMMAGE_TEST_BASH) + " -c " +
        rummage::platform::shell_argument("printf JOB_READY; IFS= read -r -n1 key")));
    REQUIRE(
        until(f.loop, [&] { return f.terminal.text().find("JOB_READY") != std::string::npos; }));
    f.terminal.request_directory(target);
    wait(f.loop, 100ms);
    // Only q is consumed by the job. The remaining bytes belong to the shell.
    f.terminal.paste(continuation ? "qprintf '%s' 'DRAFT\n" : "qtouch SHOULD_NOT_RUN #");
    wait(f.loop, 500ms);
    CHECK_FALSE(std::filesystem::exists(f.temp.path / "SHOULD_NOT_RUN"));
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK(f.terminal.directory_pending());
    CHECK_FALSE(f.terminal.ready());
    f.terminal.key(EmbeddedTerminal::Key::none, "c", true);
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && !f.terminal.directory_pending() &&
               f.terminal.directory() == target;
    }));
}

TEST_CASE("terminal: readonly continuation prompt disables unsafe automatic synchronization") {
    Fixture f;
    f.temp.write(".bashrc", "readonly PS2='more> '\n");
    REQUIRE(f.terminal.start(f.temp.path, RUMMAGE_TEST_BASH, RUMMAGE_TERMINAL_HELPER).empty());
    const auto target = f.temp.path / "next";
    std::filesystem::create_directory(target);
    f.terminal.request_directory(target);
    wait(f.loop, 400ms);
    CHECK(f.terminal.running());
    CHECK_FALSE(f.terminal.ready());
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK(f.terminal.directory_pending());
}

TEST_CASE("terminal: a delayed clean-editor acknowledgement cannot erase newer typing") {
    Fixture f;
    bool interrupt = false;
    SUBCASE("new typing") {}
    SUBCASE("interrupted probe") { interrupt = true; }
    f.start();
    const auto target = f.temp.path / "next";
    std::filesystem::create_directory(target);
    const auto signal =
        "printf '\\033]777;" + f.terminal.session_directory().filename().string() + ";clean\\007'";
    // Delay the editor's response so a keystroke deterministically arrives after
    // its empty-line check but before the parent processes the acknowledgement.
    REQUIRE(f.terminal.command("__rummage_probe() { touch probe-started; sleep 0.2; " + signal +
                               "; }"));
    REQUIRE(until(f.loop, [&] { return f.terminal.ready(); }));
    REQUIRE(f.terminal.command(
        rummage::platform::shell_argument(RUMMAGE_TEST_BASH) + " -c " +
        rummage::platform::shell_argument("printf JOB_READY; IFS= read -r -n1 key")));
    REQUIRE(
        until(f.loop, [&] { return f.terminal.text().find("JOB_READY") != std::string::npos; }));
    f.terminal.request_directory(target);
    f.terminal.key(EmbeddedTerminal::Key::none, "q");
    REQUIRE(until(f.loop, [&] { return std::filesystem::exists(f.temp.path / "probe-started"); }));
    if (interrupt)
        f.terminal.key(EmbeddedTerminal::Key::none, "c", true);
    else {
        f.terminal.key(EmbeddedTerminal::Key::none, "touch SHOULD_NOT_RUN #");
        wait(f.loop, 400ms);
        CHECK_FALSE(std::filesystem::exists(f.temp.path / "SHOULD_NOT_RUN"));
        CHECK(f.terminal.directory() == f.temp.path);
        CHECK(f.terminal.directory_pending());
        CHECK_FALSE(f.terminal.ready());
        f.terminal.key(EmbeddedTerminal::Key::none, "u", true);
        f.terminal.key(EmbeddedTerminal::Key::enter, "");
    }
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && !f.terminal.directory_pending() &&
               f.terminal.directory() == target;
    }));
}
#ifdef RUMMAGE_TEST_ZSH
TEST_CASE("terminal: zsh prompt hooks support bidirectional cwd without modifying user files") {
    Fixture f;
    std::filesystem::create_directory(f.temp.path / "zsh-config");
    f.temp.write(".zshenv", "ZDOTDIR=$HOME/zsh-config\n");
    f.temp.write("zsh-config/.zshrc", "precmd() { print CUSTOM; }\n");
    f.start(RUMMAGE_TEST_ZSH);
    CHECK(f.terminal.text().find("CUSTOM") != std::string::npos);
    const auto target = f.temp.path / "zsh folder";
    std::filesystem::create_directory(target);
    f.terminal.request_directory(target);
    REQUIRE(until(f.loop, [&] { return f.terminal.ready() && f.terminal.directory() == target; }));
    REQUIRE(f.terminal.command("cd -- " + rummage::platform::shell_argument(f.temp.path.string())));
    REQUIRE(
        until(f.loop, [&] { return f.terminal.ready() && f.terminal.directory() == f.temp.path; }));
}
#endif

TEST_CASE("terminal: failed cwd sync acknowledges failure and oversized input stays bounded") {
    Fixture f;
    f.start();
    f.terminal.request_directory(f.temp.path / "missing");
    REQUIRE(until(f.loop, [&] { return f.terminal.ready() && !f.terminal.directory_pending(); }));
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK_FALSE(f.terminal.error().empty());
    f.terminal.paste(std::string(70000, 'x'));
    CHECK(f.terminal.ready());
    CHECK_FALSE(f.terminal.command(std::string(70000, 'x')));
    CHECK(f.terminal.ready());
    REQUIRE(f.terminal.command(
        "printf "
        "'\\033]52;c;U0VDUkVU\\007\\033]8;;https://example.invalid/\\007safe\\033]8;;\\007\\n'"));
    REQUIRE(until(f.loop, [&] {
        return f.terminal.ready() && f.terminal.text().find("safe") != std::string::npos;
    }));
    CHECK(f.terminal.text().find("SECRET") == std::string::npos);
}

TEST_CASE(
    "terminal: input before the first prompt cannot authorize automatic directory injection") {
    Fixture f;
    f.temp.write(".bashrc", "sleep 0.2\nPS1='test> '\n");
    const auto target = f.temp.path / "later";
    std::filesystem::create_directory(target);
    REQUIRE(f.terminal.start(f.temp.path, RUMMAGE_TEST_BASH, RUMMAGE_TERMINAL_HELPER).empty());
    f.terminal.key(EmbeddedTerminal::Key::none, "echo first");
    f.terminal.key(EmbeddedTerminal::Key::enter, "");
    f.terminal.request_directory(target);
    wait(f.loop, 500ms);
    CHECK(f.terminal.directory() == f.temp.path);
    CHECK_FALSE(f.terminal.ready());
    f.terminal.key(EmbeddedTerminal::Key::enter, "");
    REQUIRE(until(f.loop, [&] { return f.terminal.ready() && f.terminal.directory() == target; }));
}
