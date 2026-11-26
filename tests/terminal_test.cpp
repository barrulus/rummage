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
