#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/list_size.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::ui::PointerButton;
using rummage::ui::PointerEvent;
using rummage::views::Browser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

constexpr std::int32_t kWidth = 1000;
constexpr std::int32_t kHeight = 700;

// Construct before the fixture starts workers; restore after they have stopped.
struct ScopedHome {
    std::optional<std::string> previous;

    explicit ScopedHome(const char* value) {
        if (const char* home = std::getenv("HOME")) {
            previous = home;
        }
        if (value) {
            REQUIRE(setenv("HOME", value, 1) == 0);
        } else {
            REQUIRE(unsetenv("HOME") == 0);
        }
    }

    ~ScopedHome() {
        if (previous) {
            setenv("HOME", previous->c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }
};

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

constexpr Modifiers kCtrl{.ctrl = true};
constexpr Modifiers kCtrlShift{.ctrl = true, .shift = true};

std::unique_ptr<rummage::ui::Offscreen> offscreen(std::int32_t height = kHeight) {
    try {
        return std::make_unique<rummage::ui::Offscreen>(kWidth, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// a/{one.txt, sub/}, b/{two.txt}, in the columns at a.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    fs::path b = dir.path / "b";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::size_t redraws{0};
    Browser browser{loop,
                    pool,
                    watcher,
                    rummage::fs::MimeDatabase::load(),
                    [this] { ++redraws; },
                    [](CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(a / "sub");
        fs::create_directories(b);
        std::ofstream{a / "one.txt"} << "1";
        std::ofstream{b / "two.txt"} << "2";
        browser.set_home_trash(dir.path / "Trash");
        browser.open(a);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) {
        const bool handled = browser.key(event, Browser::Clock::now());
        REQUIRE(settle());
        return handled;
    }
};

}  // namespace

TEST_CASE("panes: tab sets, navigation, selection and filters stay independent") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    auto left = f.browser.details().folder();
    f.browser.open_tab(f.a / "sub", false);
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_F3)));
    REQUIRE(f.browser.split_visible());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_count() == 1);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_show_hidden(true);
    f.browser.set_name_filter("two");
    f.browser.details().folder()->select_name("two.txt");
    auto right = f.browser.details().folder();
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK(f.browser.active_pane() == 0);
    CHECK(f.browser.tab_titles() == Names{"a", "sub"});
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::name);
    CHECK_FALSE(f.browser.shows_hidden());
    CHECK(f.browser.name_filter().empty());
    REQUIRE(left->cursor_entry());
    CHECK(left->cursor_entry()->name == "one.txt");
    CHECK(f.press(key(XKB_KEY_Tab, kCtrl)));
    CHECK(f.browser.details().folder()->path() == f.a / "sub");
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK(f.browser.tab_titles() == Names{"b"});
    CHECK(f.browser.name_filter() == "two");
    CHECK(f.browser.sort_order().descending);
    CHECK(f.browser.shows_hidden());
    REQUIRE(right->cursor_entry());
    CHECK(right->cursor_entry()->name == "two.txt");
    CHECK(f.press(key(XKB_KEY_w, kCtrl)));
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_F3)));
    CHECK_FALSE(f.browser.split_visible());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.press(key(XKB_KEY_F3)));
    f.browser.focus_pane(0);
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.browser.tab_count() == 2);
}

TEST_CASE("panes: transfers capture both ends, reverse direction, and use shared undo") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    f.browser.transfer_to_other_pane(false);
    // Change destination immediately; the queued copy still goes to b.
    f.browser.focus_pane(1);
    f.browser.open(f.a / "sub");
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK_FALSE(fs::exists(f.a / "sub" / "one.txt"));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.b / "one.txt"));
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_name("two.txt");
    f.browser.transfer_to_other_pane(true);
    REQUIRE(f.settle());
    CHECK(fs::exists(f.a / "two.txt"));
    CHECK_FALSE(fs::exists(f.b / "two.txt"));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "two.txt"));
    CHECK_FALSE(fs::exists(f.a / "two.txt"));
    f.browser.details().folder()->select_name("two.txt");
    f.browser.toggle_split();
    f.browser.transfer_to_other_pane(false);
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.a / "two.txt"));
}

TEST_CASE("panes: captured menu transfers cannot follow a later selection or destination") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    auto items = f.browser.palette_items();
    auto found =
        std::ranges::find(items, "command:copy_other_pane", &rummage::views::PaletteItem::id);
    REQUIRE(found != items.end());
    auto action = found->action;
    CHECK(found->label.find(f.b.string()) != std::string::npos);
    f.browser.details().folder()->select_name("sub");
    f.browser.focus_pane(1);
    f.browser.open(f.a / "sub");
    action();
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK_FALSE(fs::exists(f.b / "sub"));
    CHECK_FALSE(fs::exists(f.a / "sub" / "one.txt"));
}

TEST_CASE("panes: session round trip retains both tab sets even while one is hidden") {
    Fixture f;
    f.browser.open_tab(f.a / "sub", true);
    REQUIRE(f.settle());
    f.browser.toggle_split();
    f.browser.open(f.b);
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_sort(rummage::fs::SortKey::modified, true);
    f.browser.set_show_hidden(true);
    f.browser.set_name_filter("two");
    f.browser.set_stacked(true);
    f.browser.set_split_ratio(0.65F);
    f.browser.toggle_split();
    REQUIRE(f.settle());
    const auto saved = f.browser.state();
    const auto parsed = rummage::views::parse_state(rummage::views::format_state(saved), "test");
    REQUIRE(parsed.problems.empty());
    CHECK(parsed.state == saved);
    Browser restored{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    restored.apply(parsed.state);
    REQUIRE(restored.restore_session(parsed.state));
    REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
    CHECK_FALSE(restored.split_visible());
    CHECK(restored.active_pane() == 1);
    CHECK(restored.tab_titles() == Names{"b"});
    CHECK(restored.mode() == Browser::Mode::grid);
    CHECK(restored.name_filter() == "two");
    CHECK(restored.shows_hidden());
    CHECK(restored.sort_order().key == rummage::fs::SortKey::modified);
    CHECK(restored.sort_order().descending);
    CHECK(restored.stacked());
    CHECK(restored.split_ratio() == doctest::Approx(0.65F));
    restored.toggle_split();
    restored.focus_pane(0);
    CHECK(restored.tab_titles() == Names{"a", "sub"});
    CHECK(restored.active_tab() == 1);
}

TEST_CASE("panes: opening elsewhere preserves destination tabs; swapping follows content") {
    Fixture f;
    f.browser.toggle_split();
    f.browser.open(f.b);
    f.browser.focus_pane(0);
    f.browser.open_in_other_pane(f.a / "sub");
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_titles() == Names{"b", "sub"});
    f.browser.swap_panes();
    CHECK(f.browser.active_pane() == 0);
    CHECK(f.browser.tab_titles() == Names{"b", "sub"});
    f.browser.focus_pane(1);
    CHECK(f.browser.tab_titles() == Names{"a"});
}

TEST_CASE("panes: editor and filter Tab never switch panes; picker cannot split") {
    Fixture f;
    f.browser.toggle_split();
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_f, kCtrlShift)));
    CHECK(f.browser.filtering());
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK_FALSE(f.browser.filtering());
    CHECK(f.browser.active_pane() == 1);
    f.browser.columns().focused_folder()->select_name("one.txt");
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    f.browser.key(key(XKB_KEY_Tab), Browser::Clock::now());
    CHECK(f.browser.active_pane() == 1);
    f.browser.name_editor().cancel();
    Browser picker{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    picker.set_panes_enabled(false);
    picker.open(f.a);
    picker.toggle_split();
    CHECK_FALSE(picker.split_visible());
    picker.open_in_other_pane(f.b);
    CHECK(picker.tab_count() == 1);
}

TEST_CASE("panes: pointer focus, inactive scrolling, both tab bars, resizing and drop targets") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    auto left = f.browser.details().folder();
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1.0F, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    const auto click = [&](float x, float y, int count = 1) {
        f.browser.pointer({.kind = PointerEvent::Kind::press,
                           .x = x,
                           .y = y,
                           .button = PointerButton::left,
                           .click_count = count},
                          Browser::Clock::now());
        f.browser.pointer(
            {.kind = PointerEvent::Kind::release, .x = x, .y = y, .button = PointerButton::left},
            Browser::Clock::now());
    };
    draw();
    const auto l = f.browser.pane_bounds(0);
    const auto r = f.browser.pane_bounds(1);
    CHECK(l.right() < r.x);
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = l.x + 20, .y = l.y + 220},
                      Browser::Clock::now());
    f.browser.pointer(
        {.kind = PointerEvent::Kind::scroll, .x = l.x + 20, .y = l.y + 220, .wheel_y = 1},
        Browser::Clock::now());
    CHECK(f.browser.active_pane() == 1);
    click(l.x + 20, l.y + 10);
    CHECK(f.browser.active_pane() == 0);
    REQUIRE(left->cursor_entry());
    CHECK(left->cursor_entry()->name == "one.txt");
    // Use the inactive tab bar's plus button: it focuses that pane and adds only there.
    f.browser.focus_pane(1);
    const auto add = f.browser.tab_bar().add_rect();
    f.browser.focus_pane(0);
    click(add.x + 3, add.y + 3);
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_count() == 2);
    f.browser.focus_pane(0);
    CHECK(f.browser.tab_count() == 1);
    draw();
    auto divider = f.browser.divider_bounds();
    CHECK(f.browser.cursor_at(divider.x + 2, divider.y + 10) == CursorShape::ew_resize);
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = divider.x + 2,
                       .y = divider.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = 650, .y = divider.y + 10},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::release,
                       .x = 650,
                       .y = divider.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    CHECK(f.browser.split_ratio() > 0.6F);
    draw();
    divider = f.browser.divider_bounds();
    click(divider.x + 2, divider.y + 10, 2);
    CHECK(f.browser.split_ratio() == 0.5F);
    draw();
    f.browser.set_drag_and_drop(
        {.start = {},
         .cancel = {},
         .types = [] { return std::vector<std::string>{std::string{rummage::views::kUriList}}; },
         .source_actions = [] { return rummage::ui::kDndCopy; },
         .action = [] { return rummage::ui::kDndCopy; },
         .accept = {},
         .read =
             [&](std::string, rummage::views::DragAndDrop::Done done) {
                 const std::vector<fs::path> paths{f.a / "one.txt"};
                 done(rummage::views::uri_list(paths));
             },
         .finish = {},
         .modifiers = {}});
    f.browser.drop_event(
        {.kind = rummage::ui::DropEvent::Kind::drop, .x = r.x + 40, .y = r.bottom() - 50});
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK(f.browser.active_pane() == 0);
    f.browser.set_stacked(true);
    draw();
    CHECK(f.browser.pane_bounds(0).bottom() < f.browser.pane_bounds(1).y);
    divider = f.browser.divider_bounds();
    CHECK(f.browser.cursor_at(divider.x + 10, divider.y + 2) == CursorShape::ns_resize);
}

TEST_CASE("panes: a delayed mount opens in the originating tab without stealing focus") {
    Fixture f;
    rummage::views::Devices::Mounted complete;
    f.browser.set_devices(
        {.mount = [&](const std::string&, auto done) { complete = std::move(done); },
         .unmount = {},
         .eject = {}});
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    const rummage::fs::Place disk{.section = rummage::fs::Place::Section::devices,
                                  .label = "Disk",
                                  .path = {},
                                  .icon = "drive",
                                  .disk = {.volume = "test-volume", .mounted = false}};
    f.browser.mount_place(disk, true);
    REQUIRE(complete);
    f.browser.focus_pane(1);
    complete(f.a / "sub", {});
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.columns().focused_folder()->path() == f.b);
    f.browser.focus_pane(0);
    CHECK(f.browser.columns().focused_folder()->path() == f.a / "sub");
}

TEST_CASE("panes: missing saved folders use a valid fallback and retain the other group") {
    TempDir home_dir;
    std::optional<std::string> home = home_dir.path.string();
    fs::path expected = home_dir.path;
    SUBCASE("existing home") {}
    SUBCASE("missing home") {
        home = (home_dir.path / "missing").string();
        expected = "/";
    }
    SUBCASE("home is a file") {
        home = home_dir.write("file", "not a directory").string();
        expected = "/";
    }
    SUBCASE("relative home") {
        home = ".";
        expected = "/";
    }
    SUBCASE("empty home") {
        home = "";
        expected = "/";
    }
    SUBCASE("unset home") {
        home.reset();
        expected = "/";
    }
    const ScopedHome environment{home ? home->c_str() : nullptr};
    for (const std::size_t missing : {std::size_t{0}, std::size_t{1}}) {
        INFO("missing pane: " << missing);
        Fixture f;
        f.browser.toggle_split();
        f.browser.open(f.b);
        REQUIRE(f.settle());
        auto saved = f.browser.state();
        for (auto& tab : saved.tabs) {
            if (tab.pane == missing) {
                tab.folder = f.dir.path / "missing";
            }
        }
        saved.active_pane = 1 - missing;
        Browser restored{f.loop,    f.pool,
                         f.watcher, rummage::fs::MimeDatabase::load(),
                         [] {},     [](CursorShape) {}};
        restored.apply(saved);
        REQUIRE(restored.restore_session(saved));
        REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
        CHECK(restored.split_visible());
        CHECK(restored.active_pane() == 1 - missing);
        CHECK(restored.tab_count() == 1);
        REQUIRE(restored.columns().focused_folder());
        CHECK(restored.columns().focused_folder()->path() == (missing == 0 ? f.b : f.a));
        restored.focus_pane(missing);
        REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
        CHECK(restored.tab_count() == 1);
        REQUIRE(restored.columns().focused_folder());
        CHECK(restored.columns().focused_folder()->path() == expected);
        CHECK_FALSE(restored.columns().focused_folder()->model().error());
    }
}

TEST_CASE("panes: an idle split does not schedule another frame while drawing") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_info_visible(false);
    f.browser.set_mode(Browser::Mode::details);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    const auto now = Browser::Clock::now() + 2s;
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons, now);
        renderer.end_frame();
    };
    draw();
    REQUIRE(f.settle());
    draw();
    const auto redraws = f.redraws;
    draw();
    CHECK(f.redraws == redraws);
    CHECK_FALSE(f.browser.animating());
}

TEST_CASE("panes: split toolbars and status bar respect configured font metrics") {
    Fixture f;
    const auto gl = offscreen(kHeight * 2);
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.toggle_split();
    REQUIRE(f.settle());
    auto& status = f.browser.status_bar();
    status.set_text_size(32);
    for (const bool stacked : {false, true}) {
        f.browser.set_stacked(stacked);
        for (const float scale : {1.0F, 1.5F, 2.0F}) {
            for (const std::size_t index : {0U, 1U}) {
                f.browser.focus_pane(index);
                f.browser.set_name_filter("one");
                renderer.begin_frame(kWidth, kHeight * 2, scale, {});
                const float bottom = kHeight * 2 / scale;
                f.browser.draw(renderer, {0, 0, 600, bottom}, rummage::theme::Palette{}, icons,
                               Browser::Clock::now());
                renderer.end_frame();
                const auto pane = f.browser.pane_bounds(index);
                const auto field = f.browser.filter_bounds();
                const auto button = f.browser.new_folder_bounds();
                CHECK(field.x >= pane.x + 40);
                CHECK(field.width >= 48);
                CHECK(field.right() <= pane.right() - 6);
                if (!button.empty()) {
                    CHECK(field.right() + 4 <= button.x);
                    CHECK(button.width == 30);
                    CHECK(button.right() <= pane.right() - 6);
                }
                const float height = status.height(renderer);
                CHECK(status.claims(20, bottom - height + 1));
                CHECK_FALSE(status.claims(20, bottom - height - 1));
            }
        }
    }
}

TEST_CASE("panes: archive dialogs capture keys and password paste without switching panes") {
    Fixture f;
    f.browser.toggle_split();
    REQUIRE(f.settle());
    const auto active = f.browser.active_pane();
    rummage::views::SystemClipboard clipboard;
    clipboard.types = [] { return std::vector<std::string>{"text/plain"}; };
    clipboard.read = [](std::string, rummage::views::SystemClipboard::Done done) {
        done("archive secret");
    };
    f.browser.set_system_clipboard(std::move(clipboard));
    std::optional<std::string> answer;
    f.browser.password_dialog().open_password("secret.7z", false,
                                              [&](auto value) { answer = std::move(value); });
    CHECK(f.browser.has_overlay());
    f.press(key(XKB_KEY_v, kCtrl));
    CHECK(f.browser.name_filter().empty());
    f.press(key(XKB_KEY_Return));
    CHECK(answer == "archive secret");
    CHECK(f.browser.active_pane() == active);
    f.browser.show_action_result("Archive failed\nInstall unrar", true);
    f.press(key(XKB_KEY_Tab));
    CHECK(f.browser.active_pane() == active);
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.has_overlay());
    f.press(key(XKB_KEY_Tab));
    CHECK(f.browser.active_pane() != active);
}

TEST_CASE("panes: scrollbar drags stay captured without selecting or dragging files") {
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(mode);
        Fixture f;
        for (int i = 0; i < 300; ++i) {
            std::ofstream{f.b / std::format("item-{:03}.txt", i)} << i;
        }
        const auto gl = offscreen();
        REQUIRE(gl);
        rummage::ui::FontSystem fonts;
        rummage::ui::Renderer renderer{fonts};
        rummage::ui::IconSet icons{fonts};
        f.browser.set_sidebar_visible(false);
        f.browser.set_info_visible(false);
        f.browser.set_mode(mode);
        f.browser.toggle_split();
        f.browser.open(f.b);
        REQUIRE(f.settle());
        f.browser.set_mode(mode);
        const auto folder = mode == Browser::Mode::columns ? f.browser.columns().focused_folder()
                            : mode == Browser::Mode::grid  ? f.browser.grid().folder()
                                                           : f.browser.details().folder();
        REQUIRE(folder);
        folder->select_name("item-000.txt");
        const auto selection = folder->selected_names();
        bool file_drag = false;
        rummage::views::DragAndDrop dnd;
        dnd.start = [&](auto) { file_drag = true; };
        f.browser.set_drag_and_drop(std::move(dnd));
        const auto now = Browser::Clock::now();
        const auto draw = [&](auto time) {
            renderer.begin_frame(kWidth, kHeight, 1.0F, {});
            f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                           time);
            renderer.end_frame();
        };
        draw(now);
        draw(now + 2s);  // finish revealing the focused column
        const auto lane = mode == Browser::Mode::columns ? f.browser.columns().band_lane(*folder)
                          : mode == Browser::Mode::grid  ? f.browser.grid().band_lane(*folder)
                                                         : f.browser.details().band_lane(*folder);
        REQUIRE(lane);
        auto& scroller = mode == Browser::Mode::columns
                             ? f.browser.columns().column(f.browser.columns().focus()).scroller()
                         : mode == Browser::Mode::grid ? f.browser.grid().scroller()
                                                       : f.browser.details().scroller();
        REQUIRE(scroller.max_offset() > 0);
        scroller.jump_to(0);
        draw(now + 3s);
        const float x = lane->area.right() - 11;
        const float y = lane->area.y + 4;
        REQUIRE(f.browser.pane_bounds(1).contains(x, y));
        f.browser.focus_pane(0);  // pressing an inactive pane's scrollbar must focus it
        f.browser.pointer({.kind = PointerEvent::Kind::press,
                           .x = x,
                           .y = y,
                           .button = PointerButton::left,
                           .click_count = 1},
                          now + 3s);
        CHECK(f.browser.active_pane() == 1);
        CHECK(scroller.offset() == 0);
        // Crossing the pane divider must continue the original scrollbar's gesture.
        const float outside_x = f.browser.pane_bounds(0).x + 30;
        const float bottom = lane->area.bottom() + 100;
        f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = outside_x, .y = bottom},
                          now + 3s);
        CHECK(scroller.offset() == scroller.max_offset());
        CHECK(folder->selected_names() == selection);
        CHECK_FALSE(file_drag);
        f.browser.pointer({.kind = PointerEvent::Kind::release,
                           .x = outside_x,
                           .y = bottom,
                           .button = PointerButton::left},
                          now + 3s);
        f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = x, .y = y}, now + 3s);
        CHECK(scroller.offset() == scroller.max_offset());
        CHECK(folder->selected_names() == selection);
        CHECK_FALSE(file_drag);
    }
}

TEST_CASE("panes: horizontal column scrollbar drags independently of files and dividers") {
    Fixture f;
    const auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& view = f.browser.columns();
    view.open(f.a);
    REQUIRE(f.settle());
    view.key(key(XKB_KEY_Home), Browser::Clock::now());  // select sub and append its column
    REQUIRE(f.settle());
    REQUIRE(view.column_count() == 2);
    view.set_info_visible(false);
    const rummage::ui::Rect bounds{20, 30, 400, 500};
    const auto now = Browser::Clock::now();
    const auto draw = [&](auto time) {
        renderer.begin_frame(kWidth, kHeight, 1.0F, {});
        view.draw(renderer, bounds, rummage::theme::Palette{}, icons, time);
        renderer.end_frame();
    };
    draw(now);
    draw(now + 2s);
    const auto selected = view.column(0).folder().selected_names();
    const auto focus = view.focus();
    const float width = view.column(0).rows_area().width;
    const float y = bounds.bottom() - 11;
    // Track click at the start, then grab the thumb without changing the offset.
    view.pointer({.kind = PointerEvent::Kind::press,
                  .x = bounds.x + 1,
                  .y = y,
                  .button = PointerButton::left,
                  .click_count = 1},
                 now + 2s);
    view.pointer({.kind = PointerEvent::Kind::release,
                  .x = bounds.x + 1,
                  .y = y,
                  .button = PointerButton::left},
                 now + 2s);
    CHECK(view.scroll_offset() == 0);
    draw(now + 3s);
    CHECK_FALSE(view.hit(bounds.x + 4, y));
    view.pointer({.kind = PointerEvent::Kind::press,
                  .x = bounds.x + 4,
                  .y = y,
                  .button = PointerButton::left,
                  .click_count = 1},
                 now + 3s);
    CHECK(view.scroll_offset() == 0);
    view.pointer({.kind = PointerEvent::Kind::motion, .x = bounds.right() + 100, .y = y}, now + 3s);
    CHECK(view.scroll_offset() > 0);
    draw(now + 4s);
    CHECK(view.column(0).rows_area().width == width);
    CHECK(view.column(0).folder().selected_names() == selected);
    CHECK(view.focus() == focus);
    view.pointer({.kind = PointerEvent::Kind::release,
                  .x = bounds.x - 100,
                  .y = y,
                  .button = PointerButton::left},
                 now + 4s);
    CHECK(view.scroll_offset() == 0);
    draw(now + 5s);
    CHECK(view.scroll_offset() == 0);  // drawing must not glide back to the focused column
}

TEST_CASE("panes: folder preferences and last location follow the active pane") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_grid_size(4);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_sort(rummage::fs::SortKey::modified, true);
    f.browser.set_list_size(3);
    CHECK(f.browser.state().last_folder == f.b);
    f.browser.focus_pane(0);
    CHECK(f.browser.state().last_folder == f.a);
    CHECK(f.browser.mode() == Browser::Mode::grid);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::details);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::modified);
    CHECK(f.browser.details().list_size() == 3);
    f.browser.open(f.a);
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.grid().size_index() == 4);
    const auto saved = f.browser.state();
    Browser again{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    again.apply(saved);
    REQUIRE(again.restore_session(saved));
    REQUIRE(run_until(f.loop, [&] { return !again.loading(); }));
    CHECK(again.state().last_folder == f.a);
    again.focus_pane(1);
    CHECK(again.state().last_folder == f.b);
    CHECK(again.mode() == Browser::Mode::details);
    CHECK(again.details().list_size() == 3);
}

TEST_CASE("panes: unchanged views of the same folder share saved preferences across frames") {
    const auto surface = offscreen();
    if (!surface)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.toggle_split();
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_list_size(3);
    REQUIRE(f.settle());
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    draw();
    const auto saved = f.browser.state();
    for (int frame = 0; frame < 3; ++frame) {
        draw();
        const auto current = f.browser.state();
        CHECK(&current.folders.entries() == &saved.folders.entries());
    }
    f.browser.focus_pane(0);
    const auto focused = f.browser.state();
    REQUIRE(focused.folders.find(f.a));
    CHECK(focused.folders.find(f.a)->mode == rummage::views::State::Mode::grid);
    draw();
    CHECK(&f.browser.state().folders.entries() == &focused.folders.entries());
}

TEST_CASE("panes: preview below Places follows the active pane and retains its divider") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    f.browser.set_preview_in_sidebar(true);
    f.browser.set_mode(Browser::Mode::grid);
    draw();
    auto preview = f.browser.shared_preview_bounds();
    CHECK(preview.x == 0);
    CHECK(preview.width == f.browser.sidebar().width());
    CHECK(preview.height > 100);
    CHECK_FALSE(f.browser.grid().info_visible());
    CHECK(f.browser.pane_bounds(0).y == 0);  // no dedicated pane-controls row
    const auto divider = f.browser.sidebar_preview_divider_bounds();
    const auto now = Browser::Clock::now();
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = 80,
                       .y = divider.y + 2,
                       .button = PointerButton::left},
                      now);
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = 80, .y = 270}, now);
    f.browser.pointer(
        {.kind = PointerEvent::Kind::release, .x = 80, .y = 270, .button = PointerButton::left},
        now);
    draw();
    CHECK(f.browser.shared_preview_bounds().y < preview.y);
    const auto saved = f.browser.state();
    CHECK(saved.sidebar_preview_ratio == doctest::Approx(270.0F / kHeight));
    const auto restored = rummage::views::parse_state(rummage::views::format_state(saved), "state");
    CHECK(restored.problems.empty());
    CHECK(restored.state.sidebar_preview_ratio == saved.sidebar_preview_ratio);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    draw();
    CHECK(f.browser.shared_preview_bounds().x == 0);
    CHECK(f.browser.grid().folder()->path() == f.b);
    f.browser.set_stacked(true);
    draw();
    CHECK(f.browser.shared_preview_bounds().x == 0);
    f.browser.set_sidebar_visible(false);
    draw();
    CHECK(f.browser.shared_preview_bounds().x > 0);  // falls back to the right
    CHECK(f.browser.sidebar_preview_divider_bounds().empty());
    f.browser.set_info_visible(false);
    draw();
    CHECK(f.browser.shared_preview_bounds().empty());
}

TEST_CASE("panes: compact View menu and Settings remain accessible without the controls row") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_settings_handler(f.dir.path / "config.toml", [] {}, {});
    renderer.begin_frame(kWidth, kHeight, 1, {});
    f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                   Browser::Clock::now());
    renderer.end_frame();
    const auto view = f.browser.view_menu_bounds();
    REQUIRE_FALSE(view.empty());
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = view.x + 10,
                       .y = view.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::release,
                       .x = view.x + 10,
                       .y = view.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    REQUIRE(f.browser.context_menu().is_open());
    const auto& items = f.browser.context_menu().items();
    const auto split = std::ranges::find(items, "toggle_split", &rummage::views::MenuItem::id);
    REQUIRE(split != items.end());
    CHECK(split->keys == "F3");
    f.browser.context_menu().close();
    CHECK(f.press(key(XKB_KEY_comma, kCtrl)));
    CHECK(f.browser.settings_dialog().is_open());
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(f.browser.settings_dialog().is_open());
}

TEST_CASE("panes: compact controls share bottom Settings and choose the clicked pane's view") {
    Fixture f;
    const auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_settings_handler(f.dir.path / "config.toml", [] {}, {});
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.toggle_split();
    REQUIRE(f.settle());
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    const auto click = [&](rummage::ui::Rect box) {
        REQUIRE_FALSE(box.empty());
        for (const auto kind : {PointerEvent::Kind::press, PointerEvent::Kind::release})
            f.browser.pointer({.kind = kind,
                               .x = box.x + box.width / 2,
                               .y = box.y + box.height / 2,
                               .button = PointerButton::left},
                              Browser::Clock::now());
    };
    for (const bool stacked : {false, true}) {
        f.browser.set_stacked(stacked);
        draw();
        const auto settings = f.browser.settings_bounds();
        CHECK(f.browser.status_bar().claims(settings.x + 1, settings.y + 1));
        for (const std::size_t index : {0U, 1U}) {
            f.browser.focus_pane(index);
            draw();
            CHECK(f.browser.settings_bounds().x == settings.x);
            CHECK(f.browser.settings_bounds().y == settings.y);
            const auto pane = f.browser.pane_bounds(index);
            const auto field = f.browser.filter_bounds();
            CHECK(field.width > pane.width / 2);
            CHECK(f.browser.tab_bar().claims(pane.x + 10, pane.y + 10));
            CHECK(f.browser.view_menu_bounds().y == field.y);
            CHECK(f.browser.sort_bounds().y == field.y);
            CHECK(f.browser.insights_bounds().y == field.y);
        }
        // A click on the inactive pane's selector changes that pane, preserving the other.
        f.browser.focus_pane(0);
        f.browser.set_mode(Browser::Mode::columns);
        draw();
        const auto left_view = f.browser.view_menu_bounds();
        // Hover tips appear after the delay, leave focus alone, and disappear on exit.
        REQUIRE(f.settle());
        const auto pixels_below_toolbar = [&] {
            std::vector<std::uint8_t> pixels(64 * 26 * 4);
            glReadPixels(static_cast<int>(left_view.x) - 24,
                         kHeight - static_cast<int>(left_view.bottom()) - 32, 64, 26, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
        };
        draw();
        const auto before_hover = pixels_below_toolbar();
        f.browser.pointer(
            {.kind = PointerEvent::Kind::motion, .x = left_view.x + 15, .y = left_view.y + 16},
            Browser::Clock::now());
        draw();
        CHECK((pixels_below_toolbar() == before_hover));
        const auto deadline = Browser::Clock::now() + 500ms;
        REQUIRE(run_until(f.loop, [&] { return Browser::Clock::now() >= deadline; }));
        draw();
        CHECK((pixels_below_toolbar() != before_hover));
        CHECK(f.browser.active_pane() == 0);
        f.browser.pointer({.kind = PointerEvent::Kind::leave}, Browser::Clock::now());
        draw();
        CHECK((pixels_below_toolbar() == before_hover));
        f.browser.focus_pane(1);
        f.browser.set_mode(Browser::Mode::details);
        draw();
        click(left_view);
        REQUIRE(f.browser.context_menu().is_open());
        CHECK(f.browser.active_pane() == 0);
        const auto items = f.browser.context_menu().items();
        REQUIRE(items.size() >= 3);
        CHECK(items[0].checked);
        items[1].action();
        f.browser.context_menu().close();
        CHECK(f.browser.mode() == Browser::Mode::grid);
        f.browser.focus_pane(1);
        CHECK(f.browser.mode() == Browser::Mode::details);
        draw();
        click(settings);
        CHECK(f.browser.settings_dialog().is_open());
        CHECK(f.press(key(XKB_KEY_Escape)));
        CHECK_FALSE(f.browser.settings_dialog().is_open());
    }
}

TEST_CASE("panes: column focus preserves the view and a child's saved mode") {
    const auto surface = offscreen();
    REQUIRE(surface);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    const auto child = f.a / "sub";
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    for (const auto saved_mode : {Browser::Mode::grid, Browser::Mode::details}) {
        for (const bool pointer : {true, false}) {
            f.browser.open(child);
            REQUIRE(f.settle());
            f.browser.set_mode(saved_mode);
            f.browser.open(f.a);
            REQUIRE(f.settle());
            f.browser.set_mode(Browser::Mode::columns);
            f.browser.columns().focused_folder()->select_name("sub");
            REQUIRE(f.settle());
            draw();
            REQUIRE(f.browser.columns().column_count() == 2);
            if (pointer) {
                const auto bounds = f.browser.columns().column(1).rows_area();
                f.browser.pointer({.kind = PointerEvent::Kind::press,
                                   .x = bounds.x + 30,
                                   .y = bounds.y + 60,
                                   .button = PointerButton::right},
                                  Browser::Clock::now());
                CHECK(f.browser.context_menu().is_open());
                f.browser.context_menu().close();
            } else {
                REQUIRE(f.press(key(XKB_KEY_Right)));
                REQUIRE(f.press(key(XKB_KEY_Left)));
                REQUIRE(f.press(key(XKB_KEY_Right)));
            }
            CHECK(f.browser.mode() == Browser::Mode::columns);
            CHECK(f.browser.picker_folder()->path() == child);
            // A single tab is omitted from saved tab lists; add a background tab.
            f.browser.open_tab(f.b, false);
            REQUIRE(f.settle());
            draw();
            const auto saved = f.browser.state();
            REQUIRE(saved.folders.find(child));
            CHECK(saved.folders.find(child)->mode ==
                  static_cast<rummage::views::State::Mode>(saved_mode));
            Browser restored{f.loop,    f.pool,
                             f.watcher, rummage::fs::MimeDatabase::load(),
                             [] {},     [](CursorShape) {}};
            restored.apply(saved);
            REQUIRE(restored.restore_tabs(saved.tabs, saved.shown_tab));
            REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
            CHECK(restored.mode() == Browser::Mode::columns);
            CHECK(restored.state().folders.find(child)->mode ==
                  static_cast<rummage::views::State::Mode>(saved_mode));
            restored.open(child);
            REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
            CHECK(restored.mode() == saved_mode);
            // Focusing another tab and returning must not overwrite the child's saved mode.
            f.browser.select_tab(1);
            REQUIRE(f.settle());
            f.browser.select_tab(0);
            CHECK(f.browser.mode() == Browser::Mode::columns);
            f.browser.close_tab(1);
            f.browser.open(child);
            REQUIRE(f.settle());
            CHECK(f.browser.mode() == saved_mode);
        }
    }
}

TEST_CASE("panes: explicitly choosing columns replaces a child's remembered grid mode") {
    Fixture f;
    const auto child = f.a / "sub";
    f.browser.open(child);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.open(f.a);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::columns);
    f.browser.columns().focused_folder()->select_name("sub");
    REQUIRE(f.settle());
    REQUIRE(f.press(key(XKB_KEY_Right)));
    f.browser.set_mode(Browser::Mode::columns);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.open(child);
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::columns);
}

TEST_CASE("panes: resetting a folder leaves other visible tabs unchanged") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_list_size(3);
    f.browser.toggle_split();
    f.browser.open(f.a);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.reset_folder_view();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.has_custom_folder_view());
    f.browser.focus_pane(0);
    CHECK(f.browser.mode() == Browser::Mode::details);
    CHECK(f.browser.details().list_size() == 3);
}

TEST_CASE("panes: sidebar dismiss and toolbar restore preserve organization and width") {
    Fixture f;
    auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
    };
    f.browser.sidebar().set_width(250);
    f.browser.sidebar().toggle_section(rummage::fs::Place::Section::bookmarks);
    const auto collapsed = f.browser.sidebar().collapsed();
    draw();
    const auto close = f.browser.sidebar().close_bounds();
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = close.x + 10,
                       .y = close.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::release,
                       .x = close.x + 10,
                       .y = close.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    CHECK_FALSE(f.browser.sidebar_visible());
    CHECK_FALSE(f.browser.sidebar().focused());
    draw();
    // The toolbar starts immediately below the title/breadcrumb row. Find its new toggle.
    const auto toggle = f.browser.sidebar_toggle_bounds();
    REQUIRE_FALSE(toggle.empty());
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = toggle.x + 10,
                       .y = toggle.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::release,
                       .x = toggle.x + 10,
                       .y = toggle.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    CHECK(f.browser.sidebar_visible());
    CHECK(f.browser.sidebar().collapsed() == collapsed);
    CHECK(f.browser.sidebar().width() == 250);
}

TEST_CASE("panes: mounted sidebar destinations preserve the requesting pane and support windows") {
    Fixture f;
    rummage::views::Devices::Mounted complete;
    rummage::views::Devices devices;
    devices.mount = [&](const std::string&, rummage::views::Devices::Mounted done) {
        complete = std::move(done);
    };
    f.browser.set_devices(std::move(devices));
    rummage::views::Remotes remotes;
    remotes.mount = [&](const std::string&, rummage::views::Remotes::Prompts,
                        rummage::views::Remotes::Mounted done) { complete = std::move(done); };
    f.browser.set_remotes(std::move(remotes));
    rummage::fs::Place place{.section = rummage::fs::Place::Section::devices,
                             .label = "Disk",
                             .path = {},
                             .icon = "usb",
                             .disk = {.volume = "disk", .mounted = false}};
    SUBCASE("disk") {}
    SUBCASE("remote") {
        place.section = rummage::fs::Place::Section::bookmarks;
        place.uri = "sftp://server/data";
    }
    using Destination = Browser::PlaceDestination;
    f.browser.open_place(place, Destination::tab);
    REQUIRE(complete);
    f.browser.toggle_split();
    CHECK(f.browser.active_pane() == 1);
    f.browser.open(f.b);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.picker_folder()->model().loading(); }));
    complete(f.a / "sub", {});
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.picker_folder()->path() == f.b);
    f.browser.switch_pane();
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.picker_folder()->path() == f.a);
    std::vector<fs::path> windows;
    f.browser.set_new_window_handler([&](const fs::path& path) { windows.push_back(path); });
    f.browser.open_place(place, Destination::window);
    complete(f.b, {});
    CHECK(windows == std::vector<fs::path>{f.b});
    CHECK(f.browser.picker_folder()->path() == f.a);
    f.browser.open_place(place, Destination::window);
    complete({}, "permission denied");
    CHECK(windows.size() == 1);
    f.browser.set_picker_mode(true);
    complete = {};
    f.browser.open_place(place, Destination::window);
    f.browser.open_place(place, Destination::tab);
    CHECK_FALSE(complete);
}
