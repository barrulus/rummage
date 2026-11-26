#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>

#include <doctest/doctest.h>

#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;

namespace {
using rummage::views::Browser;
namespace fs = std::filesystem;

struct Environment {
    std::map<std::string, std::optional<std::string>> old;

    void set(const char* name, const std::string& value) {
        const char* previous = std::getenv(name);
        if (!old.contains(name))
            old[name] = previous ? std::optional<std::string>{previous} : std::nullopt;
        ::setenv(name, value.c_str(), 1);
    }

    ~Environment() {
        for (const auto& [name, value] : old)
            if (value)
                ::setenv(name.c_str(), value->c_str(), 1);
            else
                ::unsetenv(name.c_str());
    }
};

bool until(rummage::core::EventLoop& loop, const std::function<bool()>& done) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!done() && std::chrono::steady_clock::now() < end) {
        loop.add_timer(10ms, 0ms, [&] { loop.quit(); });
        loop.run();
    }
    return done();
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

struct Fixture {
    Environment environment;
    rummage::test::TempDir temp;
    rummage::core::EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};

    Fixture() {
        environment.set("HOME", temp.path.string());
        environment.set("SHELL", RUMMAGE_TEST_BASH);
        temp.write(".bashrc", "PS1='test> '\n");
        browser.open(temp.path);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(until(loop, [&] { return !browser.loading(); }));
    }

    auto& terminal() { return browser.terminal_panel().session(); }

    void start() {
        browser.toggle_terminal();
        REQUIRE(until(loop, [&] { return terminal().ready(); }));
    }

    bool press(xkb_keysym_t key, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        return browser.key(event, Browser::Clock::now());
    }

    void command(const std::string& script) {
        browser.terminal_workflow(3);
        REQUIRE(browser.confirm_dialog().is_open());
        browser.confirm_dialog().paste(script);
        REQUIRE(press(XKB_KEY_Return));
        REQUIRE_FALSE(browser.confirm_dialog().is_open());
    }

    void program(const std::string& name, const std::string& body) {
        fs::create_directory(temp.path / "bin");
        const auto path = temp.write("bin/" + name, "#!" RUMMAGE_TEST_BASH "\n" + body);
        REQUIRE(::chmod(path.c_str(), 0700) == 0);
        const char* old = std::getenv("PATH");
        environment.set("PATH", (temp.path / "bin").string() + ":" + (old ? old : ""));
    }
};
}  // namespace

TEST_CASE("terminal UI: shell command keeps selected paths as literal argv in current folder") {
    Fixture f;
    const std::string name = "- café ' $(touch injected)\nfile";
    f.temp.write(name, "data");
    f.browser.open(f.temp.path);
    REQUIRE(until(f.loop, [&] { return !f.browser.loading(); }));
    auto folder = f.browser.details().folder();
    const auto at = std::ranges::find(folder->entries(), name, &rummage::fs::Entry::name);
    REQUIRE(at != folder->entries().end());
    folder->set_cursor(static_cast<std::size_t>(at - folder->entries().begin()));
    f.command("printf '%s\\0' \"$PWD\" \"$@\" > result");
    REQUIRE(
        until(f.loop, [&] { return fs::exists(f.temp.path / "result") && f.terminal().ready(); }));
    CHECK(read(f.temp.path / "result") ==
          f.temp.path.string() + '\0' + (f.temp.path / name).string() + '\0');
    CHECK_FALSE(fs::exists(f.temp.path / "injected"));
    CHECK(f.browser.terminal_focused());
}

TEST_CASE("terminal UI: fzf results navigate directories and reject malformed or special files") {
    Fixture f;
    fs::create_directory(f.temp.path / "café ' folder");
    SUBCASE("valid NUL result") {
        f.program("fzf", "cat >/dev/null; printf './café '\\'' folder\\0'\n");
        f.browser.terminal_workflow(1);
        REQUIRE(until(f.loop, [&] {
            return f.browser.details().folder()->path() == f.temp.path / "café ' folder";
        }));
    }
    SUBCASE("truncated NUL result") {
        f.program("fzf", "cat >/dev/null; printf './café '\\'' folder'\n");
        f.browser.terminal_workflow(1);
        REQUIRE(until(f.loop, [&] {
            return f.terminal().ready() &&
                   f.terminal().text().find("[exit 0]") != std::string::npos;
        }));
        f.loop.add_timer(300ms, 0ms, [&] { f.loop.quit(); });
        f.loop.run();
        CHECK(f.browser.details().folder()->path() == f.temp.path);
    }
    SUBCASE("oversized output") {
        f.program("fzf", "cat >/dev/null; printf '%09000d\\0' 0\n");
        f.browser.terminal_workflow(1);
        REQUIRE(until(f.loop, [&] {
            return f.terminal().ready() &&
                   f.terminal().text().find("[exit 0]") != std::string::npos;
        }));
        f.loop.add_timer(300ms, 0ms, [&] { f.loop.quit(); });
        f.loop.run();
        CHECK(f.browser.details().folder()->path() == f.temp.path);
    }
    SUBCASE("closed session cannot apply a late result") {
        f.program("fzf", "cat >/dev/null; sleep 0.2; printf './café '\\'' folder\\0'\n");
        f.browser.terminal_workflow(1);
        REQUIRE(until(f.loop, [&] { return f.terminal().running(); }));
        f.terminal().close();
        f.browser.toggle_terminal();
        f.browser.toggle_terminal();
        REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
        f.loop.add_timer(350ms, 0ms, [&] { f.loop.quit(); });
        f.loop.run();
        CHECK(f.browser.details().folder()->path() == f.temp.path);
    }
    SUBCASE("FIFO substituted for result") {
        f.program("fzf",
                  "cat >/dev/null; for p in /proc/$$/fd/1; do result=$(readlink \"$p\"); done; rm "
                  "-- \"$result\"; mkfifo -- \"$result\"\n");
        f.browser.terminal_workflow(1);
        REQUIRE(until(f.loop, [&] {
            return f.terminal().ready() &&
                   f.terminal().text().find("[exit 0]") != std::string::npos;
        }));
        bool timer = false;
        f.loop.add_timer(300ms, 0ms, [&] {
            timer = true;
            f.loop.quit();
        });
        f.loop.run();
        CHECK(timer);
        CHECK(f.browser.details().folder()->path() == f.temp.path);
    }
}

TEST_CASE("terminal UI: zoxide query preserves shell metacharacters as one argument") {
    Fixture f;
    fs::create_directory(f.temp.path / "jump here");
    f.program("zoxide", "printf '%s\\0' \"$@\" > query-args; printf '%s\\n' \"$PWD/jump here\"\n");
    f.browser.terminal_workflow(2);
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("café ' $(touch injected)");
    f.press(XKB_KEY_Return);
    REQUIRE(until(
        f.loop, [&] { return f.browser.details().folder()->path() == f.temp.path / "jump here"; }));
    CHECK(read(f.temp.path / "query-args") ==
          std::string{"query"} + '\0' + "--" + '\0' + "café ' $(touch injected)" + '\0');
    CHECK_FALSE(fs::exists(f.temp.path / "injected"));
}

TEST_CASE(
    "terminal UI: focus consumes browser keys and panel resizes and hides without killing shell") {
    Fixture f;
    int external = 0;
    f.browser.set_terminal_handler([&](const auto&) { ++external; });
    f.start();
    CHECK(f.press(XKB_KEY_F4));
    CHECK(external == 0);
    CHECK(f.press(XKB_KEY_grave, {.ctrl = true}));
    CHECK_FALSE(f.browser.terminal_focused());
    CHECK(f.press(XKB_KEY_F4));
    CHECK(external == 1);
    f.browser.focus_terminal();
    f.terminal().key(rummage::platform::EmbeddedTerminal::Key::none, "u", true);
    f.terminal().key(rummage::platform::EmbeddedTerminal::Key::enter, "");
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    REQUIRE(f.terminal().command(
        "printf '\\033[2J\\033[H\\033[32mEmbedded terminal\\033[0m\\nUTF-8: café 世界\\n'"));
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    rummage::ui::Offscreen surface{800, 300};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    auto& panel = f.browser.terminal_panel();
    renderer.begin_frame(800, 300, 1, {});
    panel.draw(renderer, {0, 0, 800, 300}, {});
    renderer.end_frame();
    CHECK(f.terminal().screen().columns > 70);
    if (const char* snapshot = std::getenv("RUMMAGE_TERMINAL_SNAPSHOT")) {
        std::vector<std::uint8_t> pixels(800 * 300 * 4), flipped(pixels.size());
        glReadPixels(0, 0, 800, 300, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < 300; ++y)
            std::copy_n(pixels.data() + y * 800 * 4, 800 * 4, flipped.data() + (299 - y) * 800 * 4);
        const auto png =
            rummage::picture::encode_png({.width = 800, .height = 300, .rgba = std::move(flipped)});
        std::ofstream output{snapshot, std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
        REQUIRE(output.good());
    }
    panel.pointer({.kind = rummage::ui::PointerEvent::Kind::press, .x = 200, .y = 2});
    panel.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 200, .y = -80});
    panel.pointer({.kind = rummage::ui::PointerEvent::Kind::release, .x = 200, .y = -80});
    CHECK(panel.height(1000) > 260);
    CHECK(f.press(XKB_KEY_F4, {.shift = true}));
    CHECK_FALSE(panel.visible());
    CHECK(f.terminal().running());
}

TEST_CASE("terminal UI: installed fzf and zoxide navigate disposable Unicode paths") {
    // Optional end-to-end check for developer machines with both tools. Normal CI exercises
    // deterministic protocol fixtures above and does not require optional navigation tools.
    if (!std::getenv("RUMMAGE_TEST_NAVIGATION_TOOLS"))
        return;
    Fixture f;
    const auto destination = f.temp.path / "needle café ' folder";
    fs::create_directory(destination);
    f.environment.set("FZF_DEFAULT_OPTS", "--filter=needle");
    f.environment.set("_ZO_DATA_DIR", (f.temp.path / "zoxide-data").string());
    f.browser.terminal_workflow(1);
    REQUIRE(until(f.loop, [&] { return f.browser.details().folder()->path() == destination; }));
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    REQUIRE(f.terminal().command("zoxide add -- " +
                                 rummage::platform::shell_argument(destination.string())));
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    f.browser.open(f.temp.path);
    REQUIRE(until(f.loop, [&] {
        return !f.browser.loading() && f.terminal().directory() == f.temp.path &&
               f.terminal().ready();
    }));
    f.browser.terminal_workflow(2);
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("needle");
    f.press(XKB_KEY_Return);
    REQUIRE(until(f.loop, [&] { return f.browser.details().folder()->path() == destination; }));
}

TEST_CASE("terminal UI: navigation tools appear only when their executables are available") {
    Fixture f;
    const auto bin = f.temp.path / "bin";
    fs::create_directory(bin);
    f.environment.set("PATH", bin.string());
    const auto offered = [&](std::string_view id) {
        f.browser.open_palette();
        const auto& rows = f.browser.command_palette().items();
        const bool found = std::ranges::any_of(rows, [&](const auto& row) { return row.id == id; });
        f.browser.command_palette().close();
        return found;
    };
    CHECK_FALSE(offered("command:fzf_navigate"));
    CHECK_FALSE(offered("command:zoxide_jump"));
    for (const char* name : {"sh", "find", "fzf", "zoxide"}) {
        std::ofstream{bin / name} << "#!/bin/sh\nexit 0\n";
        REQUIRE(::chmod((bin / name).c_str(), 0700) == 0);
    }
    CHECK(offered("command:fzf_navigate"));
    CHECK(offered("command:zoxide_jump"));
    fs::remove(bin / "fzf");
    CHECK_FALSE(offered("command:fzf_navigate"));
    CHECK(offered("command:zoxide_jump"));
}

TEST_CASE("terminal UI: focused shell receives input while a virtual search view is active") {
    Fixture f;
    f.start();
    f.browser.open_search_results(f.temp.path, "needle", false);
    REQUIRE(f.browser.searching_results());
    REQUIRE(f.browser.terminal_focused());
    rummage::ui::KeyEvent input;
    input.keysym = XKB_KEY_p;
    input.text = "printf 'IN_TERMINAL\\n'";
    CHECK(f.browser.key(input, Browser::Clock::now()));
    CHECK(f.press(XKB_KEY_Return));
    REQUIRE(until(f.loop, [&] {
        return f.terminal().ready() && f.terminal().text().find("IN_TERMINAL") != std::string::npos;
    }));
    CHECK(f.browser.search_results()->finder()->query() == "needle");
    rummage::ui::Offscreen surface{800, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    renderer.begin_frame(800, 600, 1, {});
    f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    CHECK(f.browser.search_results()->list_bounds().bottom() <=
          f.browser.terminal_panel().bounds().y);
    const auto query_border = [&] {
        std::array<std::uint8_t, 4> pixel{};
        glReadPixels(16, 529, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return pixel;
    };
    const auto inactive_query_border = query_border();
    if (const char* snapshot = std::getenv("RUMMAGE_TERMINAL_SNAPSHOT")) {
        std::vector<std::uint8_t> pixels(800 * 600 * 4), flipped(pixels.size());
        glReadPixels(0, 0, 800, 600, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < 600; ++y)
            std::copy_n(pixels.data() + y * 800 * 4, 800 * 4, flipped.data() + (599 - y) * 800 * 4);
        const auto png =
            rummage::picture::encode_png({.width = 800, .height = 600, .rgba = std::move(flipped)});
        std::ofstream output{std::string{snapshot} + "-search.png", std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
        REQUIRE(output.good());
    }
    CHECK(f.press(XKB_KEY_grave, {.ctrl = true}));
    CHECK_FALSE(f.browser.terminal_focused());
    renderer.begin_frame(800, 600, 1, {});
    f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    CHECK(query_border() != inactive_query_border);
    CHECK(f.press(XKB_KEY_grave, {.ctrl = true}));
    CHECK(f.browser.terminal_focused());
    renderer.begin_frame(800, 600, 1, {});
    f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    CHECK(query_border() == inactive_query_border);
    CHECK(f.press(XKB_KEY_F4, {.shift = true}));
    CHECK_FALSE(f.browser.terminal_panel().visible());
}

TEST_CASE("terminal UI: IME and accessible focus follow terminal and modal ownership") {
    namespace ui = rummage::ui;
    Fixture f;
    f.start();
    REQUIRE(f.browser.terminal_focused());
    CHECK_FALSE(f.browser.input_field());
    f.browser.terminal_workflow(3);
    REQUIRE(f.browser.confirm_dialog().is_open());
    CHECK(f.browser.input_field() != nullptr);
    CHECK(f.press(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.input_field());

    f.browser.open_search_results(f.temp.path, "needle", false);
    CHECK_FALSE(f.browser.input_field());
    ui::Offscreen surface{800, 600};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    ui::AccessibleFrame frame;
    ui::TextField::focus_input(f.browser.input_field());
    const auto draw = [&] {
        frame.nodes.clear();
        renderer.begin_frame(800, 600, 1, {});
        f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
    };
    draw();
    const auto terminal =
        std::ranges::find(frame.nodes, "terminal", &rummage::core::AccessibleNode::id);
    REQUIRE(terminal != frame.nodes.end());
    CHECK(terminal->focused);
    CHECK(terminal->text == f.terminal().text());
    CHECK(std::ranges::none_of(frame.nodes,
                               [](const auto& node) { return node.editable && node.focused; }));
    f.browser.extension_output().push("Git diff output");
    CHECK_FALSE(f.browser.input_field());
    draw();
    CHECK(
        std::ranges::none_of(frame.nodes, [](const auto& node) { return node.id == "terminal"; }));
    CHECK(f.press(XKB_KEY_Escape));
    f.browser.focus_terminal();
    f.browser.search_results()->focus_query();
    CHECK(f.browser.input_field() != nullptr);
    ui::TextField::focus_input(nullptr);
}

TEST_CASE("terminal UI: focus styling yields the filter caret and border to the shell") {
    Fixture f;
    f.start();
    f.browser.focus_terminal();
    CHECK(f.press(XKB_KEY_f, {.ctrl = true, .shift = true}));
    REQUIRE(f.browser.filtering());
    rummage::ui::Offscreen surface{800, 600};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw_border = [&] {
        renderer.begin_frame(800, 600, 1, {});
        f.browser.draw(renderer, {0, 0, 800, 600}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
        const auto bounds = f.browser.filter_bounds();
        std::array<std::uint8_t, 4> pixel{};
        glReadPixels(static_cast<int>(bounds.x), 599 - static_cast<int>(bounds.y + 5), 1, 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return pixel;
    };
    const auto focused = draw_border();
    f.browser.focus_terminal();
    REQUIRE(f.browser.terminal_focused());
    CHECK(draw_border() != focused);
    f.browser.focus_terminal();
    CHECK(draw_border() == focused);
}

TEST_CASE(
    "terminal UI: oversized selection rejects the workflow without queuing a partial command") {
    Fixture f;
    for (int i = 0; i < 250; ++i)
        f.temp.write(std::string(230, 'x') + std::to_string(i), "data");
    f.browser.open(f.temp.path);
    REQUIRE(until(f.loop, [&] { return !f.browser.loading(); }));
    f.browser.details().folder()->select_all();
    f.browser.terminal_workflow(3);
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("touch SHOULD_NOT_RUN");
    f.press(XKB_KEY_Return);
    CHECK(f.browser.confirm_dialog().is_open());
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    CHECK_FALSE(fs::exists(f.temp.path / "SHOULD_NOT_RUN"));
    f.browser.confirm_dialog().close();
    f.browser.details().folder()->set_cursor(0);
    f.command("printf ok > result");
    REQUIRE(until(f.loop, [&] { return fs::exists(f.temp.path / "result"); }));
    CHECK(read(f.temp.path / "result") == "ok");
}

TEST_CASE("terminal UI: View menu exposes visibility and every placement without restarting jobs") {
    using rummage::views::MenuItem;
    using rummage::views::TerminalPlacement;
    Fixture f;
    auto menu = [&] {
        f.browser.open_view_menu(10, 10);
        const auto& items = f.browser.context_menu().items();
        const auto found = std::ranges::find(items, "terminal", &MenuItem::id);
        REQUIRE(found != items.end());
        auto result = *found;
        f.browser.context_menu().close();
        return result;
    };
    auto terminal = menu();
    REQUIRE(terminal.children.size() == 2);
    CHECK_FALSE(terminal.children[0].checked);
    CHECK(terminal.children[0].keys == "Shift+F4");
    terminal.children[0].action();
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    REQUIRE(f.terminal().command(
        "printf ready > job-started; read -r answer; printf '%s' \"$answer\" > job-result"));
    REQUIRE(until(
        f.loop, [&] { return fs::exists(f.temp.path / "job-started") && !f.terminal().ready(); }));
    const std::array placements{TerminalPlacement::bottom, TerminalPlacement::top,
                                TerminalPlacement::left, TerminalPlacement::right,
                                TerminalPlacement::floating};
    for (std::size_t i = 0; i < placements.size(); ++i) {
        terminal = menu();
        REQUIRE(terminal.children[1].children.size() == placements.size());
        terminal.children[1].children[i].action();
        CHECK(f.browser.terminal_panel().layout().placement == placements[i]);
        CHECK(f.terminal().running());
        CHECK_FALSE(f.terminal().ready());
        CHECK(menu().children[1].children[i].checked);
        CHECK(f.browser.terminal_focused());
    }
    menu().children[0].action();
    CHECK_FALSE(f.browser.terminal_panel().visible());
    CHECK(f.terminal().running());
    menu().children[0].action();
    CHECK(f.browser.terminal_panel().visible());
    f.terminal().key(rummage::platform::EmbeddedTerminal::Key::none, "survived");
    f.terminal().key(rummage::platform::EmbeddedTerminal::Key::enter, "");
    REQUIRE(until(f.loop,
                  [&] { return f.terminal().ready() && fs::exists(f.temp.path / "job-result"); }));
    CHECK(read(f.temp.path / "job-result") == "survived");
    const auto remembered = f.browser.state();
    CHECK(remembered.terminal.placement == TerminalPlacement::floating);
    Browser restored{f.loop, f.pool,     f.watcher, rummage::fs::MimeDatabase::load(),
                     [] {},  [](auto) {}};
    restored.apply(remembered);
    CHECK(restored.terminal_panel().layout() == remembered.terminal);
    CHECK_FALSE(restored.terminal_panel().visible());
    CHECK_FALSE(restored.terminal_panel().session().running());
    f.browser.terminal_panel().set_placement(TerminalPlacement::bottom);
    f.browser.open_view_menu(650, 40, true);
    auto& context = f.browser.context_menu();
    for (std::size_t n = 0; n < context.items().size(); ++n) {
        if (context.highlighted() && context.items()[*context.highlighted()].id == "terminal")
            break;
        f.press(XKB_KEY_Down);
    }
    REQUIRE(context.highlighted());
    REQUIRE(context.items()[*context.highlighted()].id == "terminal");
    REQUIRE(f.press(XKB_KEY_Right));
    REQUIRE(f.press(XKB_KEY_Down));
    REQUIRE(f.press(XKB_KEY_Right));
    REQUIRE(context.open_levels() == 3);
    REQUIRE(f.press(XKB_KEY_End));
    if (const char* prefix = std::getenv("RUMMAGE_TERMINAL_SNAPSHOT")) {
        rummage::ui::Offscreen surface{1000, 700};
        rummage::ui::FontSystem fonts;
        rummage::ui::Renderer renderer{fonts};
        rummage::ui::IconSet icons{fonts};
        renderer.begin_frame(1000, 700, 1, {});
        f.browser.draw(renderer, {0, 0, 1000, 700}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
        std::vector<std::uint8_t> pixels(1000 * 700 * 4), flipped(pixels.size());
        glReadPixels(0, 0, 1000, 700, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < 700; ++y)
            std::copy_n(pixels.data() + y * 1000 * 4, 1000 * 4,
                        flipped.data() + (699 - y) * 1000 * 4);
        const auto png = rummage::picture::encode_png(
            {.width = 1000, .height = 700, .rgba = std::move(flipped)});
        std::ofstream output{std::string{prefix} + "-menu.png", std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
        REQUIRE(output.good());
    }
    REQUIRE(f.press(XKB_KEY_Return));
    CHECK_FALSE(context.is_open());
    CHECK(f.browser.terminal_panel().layout().placement == TerminalPlacement::floating);
}

TEST_CASE("terminal UI: dock edges resize and floating header drags with captured pointer") {
    namespace ui = rummage::ui;
    using rummage::views::TerminalPlacement;
    rummage::core::EventLoop loop;
    rummage::views::TerminalPanel panel{loop, [] {}};
    panel.show(true);
    const ui::Rect available{20, 40, 1000, 700};
    const std::array placements{TerminalPlacement::bottom, TerminalPlacement::top,
                                TerminalPlacement::left, TerminalPlacement::right,
                                TerminalPlacement::floating};
    for (const auto placement : placements) {
        CAPTURE(static_cast<int>(placement));
        panel.set_layout({.placement = placement});
        const auto remaining = panel.arrange(available);
        const auto before = panel.bounds();
        float x = before.x + 2, y = before.y + 2, dx = 0, dy = 0;
        switch (placement) {
            case TerminalPlacement::bottom:
                dy = -70;
                CHECK(remaining.bottom() == before.y);
                break;
            case TerminalPlacement::top:
                y = before.bottom() - 2;
                dy = 70;
                CHECK(remaining.y == before.bottom());
                break;
            case TerminalPlacement::left:
                x = before.right() - 2;
                dx = 70;
                CHECK(remaining.x == before.right());
                break;
            case TerminalPlacement::right:
                dx = -70;
                CHECK(remaining.right() == before.x);
                break;
            case TerminalPlacement::floating:
                x = before.right() - 2;
                y = before.bottom() - 2;
                dx = 70;
                dy = 70;
                CHECK(remaining.width == available.width);
                CHECK(remaining.height == available.height);
                break;
        }
        const auto cursor = panel.cursor_at(x, y);
        CHECK(cursor ==
              (placement == TerminalPlacement::floating ? ui::CursorShape::se_resize
               : placement == TerminalPlacement::left || placement == TerminalPlacement::right
                   ? ui::CursorShape::ew_resize
                   : ui::CursorShape::ns_resize));
        CHECK(panel.pointer({.kind = ui::PointerEvent::Kind::press,
                             .x = x,
                             .y = y,
                             .button = ui::PointerButton::left}));
        CHECK(panel.claims(-100, -100));
        CHECK(panel.pointer({.kind = ui::PointerEvent::Kind::motion, .x = x + dx, .y = y + dy}));
        panel.arrange(available);
        if (dx != 0)
            CHECK(panel.bounds().width == before.width + 70);
        if (dy != 0)
            CHECK(panel.bounds().height == before.height + 70);
        CHECK(panel.pointer({.kind = ui::PointerEvent::Kind::release,
                             .x = x + dx,
                             .y = y + dy,
                             .button = ui::PointerButton::left}));
        CHECK_FALSE(panel.claims(-100, -100));
    }
    panel.set_layout({.placement = TerminalPlacement::floating});
    panel.arrange(available);
    const auto before = panel.bounds();
    const auto x = before.x + 100, y = before.y + 15;
    CHECK(panel.cursor_at(x, y) == ui::CursorShape::grab);
    CHECK(panel.pointer({.kind = ui::PointerEvent::Kind::press,
                         .x = x,
                         .y = y,
                         .button = ui::PointerButton::left}));
    CHECK(panel.cursor_at(-100, -100) == ui::CursorShape::grabbing);
    panel.pointer({.kind = ui::PointerEvent::Kind::motion, .x = x + 100, .y = y + 80});
    panel.arrange(available);
    CHECK(panel.bounds().x == before.x + 100);
    CHECK(panel.bounds().y == before.y + 80);
    panel.pointer({.kind = ui::PointerEvent::Kind::motion, .x = -500, .y = -500});
    panel.arrange(available);
    CHECK(panel.bounds().x == available.x);
    CHECK(panel.bounds().y == available.y);
    panel.pointer({.kind = ui::PointerEvent::Kind::release,
                   .x = -500,
                   .y = -500,
                   .button = ui::PointerButton::left});
    CHECK(panel.focused());
    panel.arrange({0, 0, 80, 60});
    CHECK(panel.bounds().width == 80);
    CHECK(panel.bounds().height == 60);
}

TEST_CASE("terminal UI: all placements render above search and retain terminal input focus") {
    namespace ui = rummage::ui;
    using rummage::views::TerminalPlacement;
    Fixture f;
    f.start();
    REQUIRE(f.terminal().command(
        "printf '\\033[2J\\033[H\\033[32mTerminal placement\\033[0m\\nUTF-8: café 世界\\n'"));
    REQUIRE(until(f.loop, [&] { return f.terminal().ready(); }));
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.open_search_results(f.temp.path, "needle", false);
    ui::Offscreen surface{1000, 700};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const std::array placements{TerminalPlacement::bottom, TerminalPlacement::top,
                                TerminalPlacement::left, TerminalPlacement::right,
                                TerminalPlacement::floating};
    for (const auto placement : placements) {
        CAPTURE(static_cast<int>(placement));
        auto& panel = f.browser.terminal_panel();
        panel.set_placement(placement);
        renderer.begin_frame(1000, 700, 1, {});
        f.browser.draw(renderer, {0, 0, 1000, 700}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
        const auto bounds = panel.bounds(), list = f.browser.search_results()->list_bounds();
        switch (placement) {
            case TerminalPlacement::bottom:
                CHECK(list.bottom() <= bounds.y);
                break;
            case TerminalPlacement::top:
                CHECK(list.y >= bounds.bottom());
                break;
            case TerminalPlacement::left:
                CHECK(list.x >= bounds.right());
                break;
            case TerminalPlacement::right:
                CHECK(list.right() <= bounds.x);
                break;
            case TerminalPlacement::floating:
                CHECK(list.width > bounds.width);
                break;
        }
        f.browser.pointer({.kind = ui::PointerEvent::Kind::press,
                           .x = bounds.x + 20,
                           .y = bounds.y + 60,
                           .button = ui::PointerButton::left},
                          Browser::Clock::now());
        CHECK(f.browser.terminal_focused());
        CHECK_FALSE(f.browser.input_field());
        if (const char* prefix = std::getenv("RUMMAGE_TERMINAL_SNAPSHOT")) {
            std::vector<std::uint8_t> pixels(1000 * 700 * 4), flipped(pixels.size());
            glReadPixels(0, 0, 1000, 700, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (int y = 0; y < 700; ++y)
                std::copy_n(pixels.data() + y * 1000 * 4, 1000 * 4,
                            flipped.data() + (699 - y) * 1000 * 4);
            const auto png = rummage::picture::encode_png(
                {.width = 1000, .height = 700, .rgba = std::move(flipped)});
            std::ofstream output{std::string{prefix} + "-placement-" +
                                     std::to_string(static_cast<int>(placement)) + ".png",
                                 std::ios::binary};
            output.write(reinterpret_cast<const char*>(png.data()),
                         static_cast<std::streamsize>(png.size()));
            REQUIRE(output.good());
        }
    }
}
