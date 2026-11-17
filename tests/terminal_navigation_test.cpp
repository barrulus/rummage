#include "rummage/platform/terminal_navigation.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

namespace {
namespace platform = rummage::platform;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string quoted(const fs::path& path) {
    std::string out = "'";
    for (char c : path.string())
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

bool send(const fs::path& session, const std::string& message) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);
    sockaddr_un target{};
    target.sun_family = AF_UNIX;
    const auto path = (session / "handoff").string();
    REQUIRE(path.size() < sizeof(target.sun_path));
    std::memcpy(target.sun_path, path.c_str(), path.size() + 1);
    const auto size = sendto(fd, message.data(), message.size(), 0,
                             reinterpret_cast<sockaddr*>(&target), sizeof(target));
    close(fd);
    return size == static_cast<ssize_t>(message.size());
}

struct Fixture {
    rummage::test::TempDir temp;
    rummage::core::EventLoop loop;
    platform::Environment environment = platform::Environment::from_environment();
    std::vector<fs::path> returned;
    std::vector<std::string> errors;
    platform::TerminalNavigation navigation{loop,
                                            [&](const auto& path) { returned.push_back(path); },
                                            [&](const auto& message, bool error) {
                                                if (error)
                                                    errors.push_back(message);
                                            }};

    void terminal(const std::string& body) {
        const auto sh = platform::find_program("sh", environment.path);
        REQUIRE(sh);
        const auto script = temp.write("terminal", "#!" + sh->string() + "\n" + body);
        REQUIRE(chmod(script.c_str(), 0700) == 0);
        environment.configured_terminal = {script.string()};
    }

    void start(std::string shell = "bash") {
        navigation.start(temp.path, environment, shell, RUMMAGE_EXECUTABLE);
    }

    bool until(const std::function<bool()>& done) {
        bool ready = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            if (done()) {
                ready = true;
                loop.quit();
            }
        });
        const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(poll);
        loop.remove(timeout);
        return ready;
    }
};
}  // namespace

TEST_CASE(
    "terminal navigation: one-shot private sessions remain independent and reject late handoffs") {
    Fixture a;
    Fixture b;
    a.terminal("exit 0\n");  // a terminal client can exit while its server still runs the shell
    b.terminal("exit 0\n");
    a.start();
    b.start();
    REQUIRE(a.navigation.active());
    REQUIRE(b.navigation.active());
    const auto first = a.navigation.session_directory();
    const auto second = b.navigation.session_directory();
    CHECK(first != second);
    struct stat info{};
    REQUIRE(stat(first.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
    CHECK(info.st_uid == getuid());
    const auto target = a.temp.path / "space ' café\n\n";
    fs::create_directory(target);
    REQUIRE(send(first, target.string()));
    REQUIRE(a.until([&] { return !a.navigation.active(); }));
    CHECK(a.returned == std::vector<fs::path>{target});
    CHECK(b.returned.empty());
    CHECK_FALSE(fs::exists(first));
    CHECK_FALSE(send(first, b.temp.path.string()));
    b.navigation.cancel();
    CHECK_FALSE(fs::exists(second));
    CHECK_FALSE(send(second, target.string()));
}

TEST_CASE("terminal navigation: invalid and inaccessible directory messages preserve the picker") {
    Fixture f;
    f.terminal("exit 0\n");
    std::string message;
    SUBCASE("relative") { message = "relative"; }
    SUBCASE("missing") { message = (f.temp.path / "gone").string(); }
    SUBCASE("file") { message = f.temp.write("file", "file").string(); }
    SUBCASE("nul") { message = f.temp.path.string() + std::string{"\0junk", 5}; }
    SUBCASE("oversized") { message = "/" + std::string(9000, 'x'); }
    SUBCASE("unreadable") {
        const auto blocked = f.temp.path / "blocked";
        fs::create_directory(blocked);
        REQUIRE(chmod(blocked.c_str(), 0000) == 0);
        message = blocked.string();
        if (getuid() == 0)
            return;
    }
    f.start();
    REQUIRE(f.navigation.active());
    REQUIRE(send(f.navigation.session_directory(), message));
    REQUIRE(f.until([&] { return !f.navigation.active(); }));
    CHECK(f.returned.empty());
    REQUIRE(f.errors.size() == 1);
}

TEST_CASE("terminal navigation: unsupported shells and failed terminal launches clean up") {
    Fixture f;
    SUBCASE("unsupported shell") { f.start("dash"); }
    SUBCASE("missing terminal") {
        f.environment.path.clear();
        f.environment.terminal.clear();
        const auto bash =
            platform::find_program("bash", platform::Environment::from_environment().path);
        REQUIRE(bash);
        f.start(bash->string());
    }
    SUBCASE("failed exec") {
        const auto terminal = f.temp.write("terminal", "#!/no-such-interpreter\n");
        REQUIRE(chmod(terminal.c_str(), 0700) == 0);
        f.environment.configured_terminal = {terminal.string()};
        f.start();
    }
    SUBCASE("terminal exits unsuccessfully") {
        f.terminal("exit 42\n");
        f.start();
        REQUIRE(f.until([&] { return !f.navigation.active(); }));
    }
    CHECK_FALSE(f.navigation.active());
    CHECK(f.navigation.session_directory().empty());
    CHECK(f.returned.empty());
    CHECK_FALSE(f.errors.empty());
}

namespace {
void check_shell(const std::string& shell) {
    Fixture f;
    if (!platform::find_program(shell, f.environment.path)) {
        WARN_MESSAGE(false, "Optional shell is unavailable: ", shell);
        return;
    }
    const auto target = f.temp.path / "space ' café\n\n";
    fs::create_directory(target);
    const auto hook = f.temp.path / "user-exit";
    auto rc = "function croot() { cd -- " + quoted(target) + "; }\n" + "trap 'printf user-exit > " +
              hook.string() + "' EXIT\n";
    f.temp.write(".bashrc", rc);
    f.temp.write(".zshrc", rc);
    // No ZDOTDIR in the parent in the normal CI environment. If set, supply a matching
    // .zshenv in that location would modify the developer's files, so use a child env test.
    if (shell == "zsh" && std::getenv("ZDOTDIR")) {
        WARN_MESSAGE(false, "Zsh integration test requires an unset ZDOTDIR");
        return;
    }
    std::string ending = "exit 7\n";
    bool normal = true;
    bool hup_hook = false;
    SUBCASE("explicit nonzero exit") {}
    SUBCASE("explicit exit with signal-like status") { ending = "exit 129\n"; }
    SUBCASE("existing hangup trap") {
        rc += "trap 'printf handled > " + (f.temp.path / "user-hup").string() + "; exit 0' HUP\n";
        ending = "kill -HUP $$\n";
        normal = false;
        hup_hook = true;
    }
    SUBCASE("EOF") { ending.clear(); }
    SUBCASE("errexit startup configuration") { rc += "set -e\n"; }
    SUBCASE("zshenv relocates startup configuration") {
        if (shell == "zsh") {
            const auto relocated = f.temp.path / "zsh config";
            fs::create_directory(relocated);
            f.temp.write(".zshenv", "export ZDOTDIR=" + quoted(relocated) + "\n");
            f.temp.write("zsh config/.zshrc", rc);
        }
    }
    SUBCASE("zsh function-style hangup trap") {
        if (shell == "zsh") {
            rc += "TRAPHUP() { printf handled > " + (f.temp.path / "user-hup").string() +
                  "; exit 0; }\n";
            ending = "kill -HUP $$\n";
            normal = false;
            hup_hook = true;
        }
    }
    SUBCASE("hangup") {
        ending = "kill -HUP $$\n";
        normal = false;
    }
    SUBCASE("killed") {
        ending = "kill -KILL $$\n";
        normal = false;
    }
    SUBCASE("integration replaced") {
        ending = "exec sh -c 'exit 0'\n";
        normal = false;
    }
    f.temp.write(".bashrc", rc);
    f.temp.write(".zshrc", rc);
    const auto input = f.temp.write("input", "croot\n" + ending);
    f.terminal("shift\nexport HOME=" + quoted(f.temp.path) +
               "\n"
               "exec \"$@\" < " +
               quoted(input) + " > " + quoted(f.temp.path / "output") + " 2>&1\n");
    f.start(shell);
    REQUIRE(f.until([&] { return !f.navigation.active(); }));
    std::ifstream output{f.temp.path / "output"};
    const std::string log{std::istreambuf_iterator<char>{output}, {}};
    INFO(log);
    if (hup_hook)
        CHECK(fs::exists(f.temp.path / "user-hup"));
    if (normal) {
        CHECK(f.errors.empty());
        CHECK(f.returned == std::vector<fs::path>{target});
        CHECK(fs::exists(hook));
    } else {
        CHECK(f.returned.empty());
        CHECK_FALSE(f.errors.empty());
    }
}

}  // namespace

TEST_CASE("terminal navigation: Bash keeps user config and reports only normal shell exits") {
    check_shell("bash");
}

TEST_CASE("terminal navigation: Zsh keeps user config and reports only normal shell exits") {
    check_shell("zsh");
}

TEST_CASE("terminal navigation: Fish loads normal config and reports only normal shell exits") {
    Fixture f;
    if (!platform::find_program("fish", f.environment.path)) {
        WARN_MESSAGE(false, "Optional shell is unavailable: fish");
        return;
    }
    const auto target = f.temp.path / "space ' café\\\n\n";
    const auto config = f.temp.path / "config with spaces";
    fs::create_directory(target);
    fs::create_directories(config / "fish/conf.d");
    fs::create_directories(config / "fish/functions");
    // Put the target in an environment variable: Fish must preserve every filename byte.
    f.temp.write("config with spaces/fish/functions/croot.fish",
                 "function croot; builtin cd -- \"$RUMMAGE_TEST_TARGET\"; end\n");
    f.temp.write("config with spaces/fish/conf.d/navigation.fish",
                 "set -g rummage_test_snippet loaded\n");
    const auto exit_hook = f.temp.path / "user-exit";
    std::string rc =
        "function user_exit --on-event fish_exit\n"
        "    printf '%s' $rummage_test_snippet > \"$HOME/user-exit\"\n"
        "end\n";
    bool normal = true;
    bool signal_hook = false;
    bool cancel = false;
    std::string signal_value = "handled";
    std::string ending = "exit 7\n";
    SUBCASE("explicit nonzero exit") {}
    SUBCASE("exit with signal-like status") { ending = "exit 129\n"; }
    SUBCASE("EOF") { ending.clear(); }
    SUBCASE("hangup") {
        ending = "kill -HUP $fish_pid\n";
        normal = false;
    }
    SUBCASE("termination") {
        ending = "kill -TERM $fish_pid\n";
        normal = false;
    }
    SUBCASE("killed") {
        ending = "kill -KILL $fish_pid\n";
        normal = false;
    }
    SUBCASE("replaced shell") {
        ending = "exec sh -c 'exit 0'\n";
        normal = false;
    }
    SUBCASE("existing signal handler exits successfully") {
        rc +=
            "set -l captured handled\n"
            "function user_hup --on-signal HUP --inherit-variable captured\n"
            "    printf '%s' $captured > \"$HOME/user-signal\"\n"
            "    exit 0\n"
            "end\n";
        ending = "kill -HUP $fish_pid\n";
        normal = false;
        signal_hook = true;
    }
    SUBCASE("existing termination handler exits successfully") {
        rc +=
            "function user_term --on-signal TERM\n"
            "    printf handled > \"$HOME/user-signal\"\n"
            "    exit 0\n"
            "end\n";
        ending = "kill -TERM $fish_pid\n";
        normal = false;
        signal_hook = true;
    }
    SUBCASE("signal handler retains multiline strings and other event bindings") {
        rc +=
            "function user_hup --on-signal HUP --on-event test-other\n"
            "    set -l text 'first\n\nlast'\n"
            "    printf '%s' \"$text\" > \"$HOME/user-signal\"\n"
            "end\n";
        ending = "emit test-other\nexit 7\n";
        signal_hook = true;
        signal_value = "first\n\nlast";
    }
    SUBCASE("picker cancelled while shell stays open") {
        ending =
            "echo ready > \"$HOME/ready\"\n"
            "while not test -f \"$HOME/release\"; sleep 0.02; end\n"
            "exit\n";
        normal = false;
        cancel = true;
    }
    const auto startup = f.temp.write("config with spaces/fish/config.fish", rc);
    const auto input = f.temp.write("input", "croot\n" + ending);
    f.terminal(
        "shift\nexport HOME=" + quoted(f.temp.path) + "\nexport XDG_CONFIG_HOME=" + quoted(config) +
        "\nexport RUMMAGE_TEST_TARGET=" + quoted(target) + "\n\"$@\" < " + quoted(input) + " > " +
        quoted(f.temp.path / "output") + " 2>&1\n: > " + quoted(f.temp.path / "finished") + "\n");
    f.start("fish");
    REQUIRE(f.navigation.active());
    const auto session = f.navigation.session_directory();
    if (cancel) {
        REQUIRE(f.until([&] { return fs::exists(f.temp.path / "ready"); }));
        f.navigation.cancel();
        CHECK_FALSE(fs::exists(session));
        f.temp.write("release", "");
    }
    REQUIRE(
        f.until([&] { return !f.navigation.active() && fs::exists(f.temp.path / "finished"); }));
    std::ifstream output{f.temp.path / "output"};
    const std::string log{std::istreambuf_iterator<char>{output}, {}};
    INFO(log);
    if (signal_hook) {
        std::ifstream file{f.temp.path / "user-signal"};
        const std::string value{std::istreambuf_iterator<char>{file}, {}};
        CHECK(value == signal_value);
    }
    if (normal) {
        CHECK(f.errors.empty());
        CHECK(f.returned == std::vector<fs::path>{target});
        std::ifstream file{exit_hook};
        std::string value;
        file >> value;
        CHECK(value == "loaded");
    } else {
        CHECK(f.returned.empty());
        if (!cancel)
            CHECK_FALSE(f.errors.empty());
    }
    CHECK_FALSE(fs::exists(session));
    std::ifstream file{startup};
    CHECK(std::string{std::istreambuf_iterator<char>{file}, {}} == rc);
}
