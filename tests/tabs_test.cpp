#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
#include "rummage/fs/trash.hpp"
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
constexpr Modifiers kAlt{.alt = true};

std::unique_ptr<rummage::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<rummage::ui::Offscreen>(kWidth, kHeight);
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
    Browser browser;

    explicit Fixture(bool thumbnails = false)
        : browser{loop,
                  pool,
                  watcher,
                  rummage::fs::MimeDatabase::load(),
                  [] {},
                  [](CursorShape /*shape*/) {},
                  thumbnails
                      ? std::make_shared<rummage::thumbs::Cache>(dir.path / "cache", "views-test")
                      : nullptr} {
        fs::create_directories(a / "sub");
        fs::create_directories(b);
        std::ofstream{a / "one.txt"} << "1";
        std::ofstream{b / "two.txt"} << "2";
        browser.set_home_trash(dir.path / "Trash");
        browser.open(a);
        REQUIRE(settle());
    }

    const std::vector<rummage::views::MenuItem>& visibility_items() {
        const auto& items = browser.context_menu().items();
        const auto show = std::ranges::find(items, "show", &rummage::views::MenuItem::id);
        REQUIRE(show != items.end());
        CHECK(show->label == "Show");
        CHECK(show->submenu);
        CHECK(show->children.size() == 5);
        for (const auto& item : show->children)
            CHECK(item.checkable);
        return show->children;
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

TEST_CASE("tabs: Ctrl+T, Ctrl+Tab, Ctrl+Shift+Tab, Alt+1…9, Ctrl+W; the last tab stays") {
    Fixture f;
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_w, kCtrl)));  // the last tab: nothing
    CHECK(f.browser.tab_count() == 1);

    // A new tab on the folder shown, shown at once.
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.browser.tab_titles() == Names{"a", "a"});
    f.browser.open(f.b);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_titles() == Names{"a", "b"});

    // Round and round.
    CHECK(f.press(key(XKB_KEY_Tab, kCtrl)));
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.columns().focused_folder()->path() == f.a);
    CHECK(f.press(key(XKB_KEY_ISO_Left_Tab, kCtrlShift)));
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.press(key(XKB_KEY_Tab, kCtrl)));
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.press(key(XKB_KEY_2, kAlt)));
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.press(key(XKB_KEY_9, kAlt)));  // no ninth tab: nothing
    CHECK(f.browser.active_tab() == 1);

    // Closing the tab shown: the one before comes when it was the last.
    CHECK(f.press(key(XKB_KEY_w, kCtrl)));
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.tab_titles() == Names{"a"});
}

TEST_CASE("tabs: each has its view and history; sorting and hidden files are independent") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));  // in the grid too
    CHECK(f.browser.mode() == Browser::Mode::grid);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    CHECK(f.browser.history().size() == 2);  // a, then b

    f.browser.select_tab(0);
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.history().size() == 1);
    CHECK(f.browser.grid().folder()->path() == f.a);

    // Settings stay in this tab; new tabs inherit the tab they are opened from.
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_show_hidden(true);
    f.browser.select_tab(1);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::name);
    CHECK_FALSE(f.browser.shows_hidden());
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    CHECK(f.browser.sort_order().descending);
    CHECK(f.browser.details().folder()->order().key == rummage::fs::SortKey::size);
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.browser.details().order().key == rummage::fs::SortKey::size);
    CHECK(f.browser.details().folder()->order().descending);

    // Back in the second tab goes through its own history only.
    f.browser.select_tab(1);
    CHECK(f.press(key(XKB_KEY_Left, kAlt)));
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.grid().folder()->path() == f.a);
    f.browser.select_tab(0);
    CHECK(f.browser.history_index() == 0);
}

TEST_CASE("tabs: opened behind the one shown, right after it; a closed tab gives its place") {
    Fixture f;
    f.browser.open_tab(f.b, false);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 0);  // still here
    CHECK(f.browser.status_bar().message() == "Opened “b” in a new tab");
    f.browser.open_tab(f.a / "sub", false);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_titles() == Names{"a", "sub", "b"});
    CHECK(f.browser.active_tab() == 0);

    // Closing the tab shown: the one after it takes its place.
    f.browser.select_tab(1);
    f.browser.close_tab(1);
    CHECK(f.browser.tab_titles() == Names{"a", "b"});
    CHECK(f.browser.active_tab() == 1);
    // A tab before the one shown: the one shown stays.
    f.browser.close_tab(0);
    CHECK(f.browser.tab_titles() == Names{"b"});
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.columns().focused_folder()->path() == f.b);
}

TEST_CASE("tabs: Open in New Tab, one folder or several, behind the tab shown") {
    Fixture f;
    fs::create_directories(f.a / "more");
    REQUIRE(run_until(f.loop, [&f] {
        const auto& entries = f.browser.columns().focused_folder()->entries();
        return std::ranges::find(entries, "more", &rummage::fs::Entry::name) != entries.end();
    }));
    const auto select = [&f](const Names& names) {
        const std::shared_ptr<rummage::views::Folder> folder = f.browser.columns().focused_folder();
        folder->set_cursor(std::nullopt);
        for (const std::string& name : names) {
            const auto& entries = folder->entries();
            const auto it = std::ranges::find(entries, name, &rummage::fs::Entry::name);
            REQUIRE(it != entries.end());
            folder->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                               rummage::views::SelectMode::toggle);
        }
    };
    // The label of the menu's item called id (empty: none), and choosing it.
    const auto label = [&f](const std::string& id) {
        for (const rummage::views::MenuItem& each : f.browser.context_menu().items()) {
            if (each.id == id) {
                return each.label;
            }
        }
        return std::string{};
    };
    const auto choose = [&f](const std::string& id) {
        std::function<void()> action;
        for (const rummage::views::MenuItem& each : f.browser.context_menu().items()) {
            if (each.id == id) {
                action = each.action;
            }
        }
        f.browser.context_menu().close();
        REQUIRE(action);
        action();
        REQUIRE(f.settle());
    };

    // A file: no such item. A folder: one tab.
    select({"one.txt"});
    f.browser.open_menu(100, 100, false);
    CHECK(label("open_tab").empty());
    f.browser.context_menu().close();
    select({"sub"});
    f.browser.open_menu(100, 100, false);
    CHECK(label("open_tab") == "Open in New Tab");
    choose("open_tab");
    CHECK(f.browser.tab_titles() == Names{"a", "sub"});
    CHECK(f.browser.active_tab() == 0);

    // Two folders: two tabs, in their order, right after the one shown.
    select({"more", "sub"});
    f.browser.open_menu(100, 100, false);
    CHECK(label("open_tab") == "Open in New Tabs");
    choose("open_tab");
    CHECK(f.browser.tab_titles() == Names{"a", "more", "sub", "sub"});
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.status_bar().message() == "Opened 2 folders in new tabs");
}

TEST_CASE("tabs: closed while a name is edited, the rename still lands") {
    Fixture f;
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.press(key(XKB_KEY_Down)));  // the cursor on sub (folders first)
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    f.browser.key(key(XKB_KEY_End), Browser::Clock::now());
    KeyEvent typed = key(XKB_KEY_2);
    typed.text = "2";
    f.browser.key(typed, Browser::Clock::now());
    // Ctrl+W is the field's meanwhile; the tab's × closes it, keeping the name typed. Its columns
    // go with it: the folder, held by the rename, reports to nothing.
    f.browser.key(key(XKB_KEY_w, kCtrl), Browser::Clock::now());
    CHECK(f.browser.tab_count() == 2);
    f.browser.close_tab(1);
    CHECK(f.browser.tab_count() == 1);
    REQUIRE(run_until(f.loop, [&f] { return fs::exists(f.a / "sub2"); }));
    REQUIRE(f.settle());
    CHECK(f.browser.columns().focused_folder()->path() == f.a);

    // A folder kept past its tab: moving its cursor tells no columns (ASan would see them gone).
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    const std::shared_ptr<rummage::views::Folder> held = f.browser.columns().focused_folder();
    f.browser.close_tab(1);
    held->set_cursor(0);
    CHECK(held->cursor() == 0);
}

TEST_CASE("tabs: the bar with two tabs; a click shows one, its × or a middle click closes it") {
    Fixture f;
    const std::unique_ptr<rummage::ui::Offscreen> gl = offscreen();
    if (!gl) {
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1.0F, {});
        f.browser.draw(renderer, {0.0F, 0.0F, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    const auto click = [&f](const rummage::ui::Rect& rect, PointerButton button) {
        const float x = rect.x + (rect.width / 2.0F);
        const float y = rect.y + (rect.height / 2.0F);
        f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = x, .y = y},
                          Browser::Clock::now());
        f.browser.pointer(
            {.kind = PointerEvent::Kind::press, .x = x, .y = y, .button = button, .click_count = 1},
            Browser::Clock::now());
        f.browser.pointer({.kind = PointerEvent::Kind::release, .x = x, .y = y, .button = button},
                          Browser::Clock::now());
    };
    const rummage::views::TabBar& bar = f.browser.tab_bar();

    draw();
    CHECK(bar.tab_rects().empty());  // one tab: no bar
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    draw();
    REQUIRE(bar.tab_rects().size() == 2);
    const rummage::ui::Rect first = bar.tab_rects()[0];
    CHECK(f.browser.cursor_at(first.x + 20.0F, first.y + 10.0F) == CursorShape::pointer);
    // The path and the view moved down under the bar.
    CHECK(f.browser.breadcrumb().claims(first.x + 80.0F, first.bottom() + 5.0F));

    click(first, PointerButton::left);
    CHECK(f.browser.active_tab() == 0);
    draw();
    click(bar.close_rects()[1], PointerButton::left);  // the second tab's ×
    CHECK(f.browser.tab_count() == 1);
    draw();
    CHECK(bar.tab_rects().empty());

    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    draw();
    click(bar.tab_rects()[0], PointerButton::middle);
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.active_tab() == 0);

    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    draw();
    click(bar.add_rect(), PointerButton::left);  // the +
    CHECK(f.browser.tab_count() == 3);
    CHECK(f.browser.active_tab() == 2);

    // A middle click on a folder in a view: a tab behind; on a file, nothing.
    f.browser.close_tab(2);
    f.browser.close_tab(1);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    draw();
    const auto row_of = [&f](const std::string& name) -> std::optional<rummage::ui::Rect> {
        for (float y = 1.0F; y < kHeight; y += 2.0F) {
            for (float x = 300.0F; x < kWidth; x += 40.0F) {
                if (const auto hit = f.browser.details().hit(x, y);
                    hit && hit->entry && hit->folder->entries()[*hit->entry].name == name) {
                    return rummage::ui::Rect{x, y, 1.0F, 1.0F};
                }
            }
        }
        return std::nullopt;
    };
    const std::optional<rummage::ui::Rect> sub = row_of("sub");
    const std::optional<rummage::ui::Rect> one = row_of("one.txt");
    REQUIRE(sub);
    REQUIRE(one);
    click(one.value_or(rummage::ui::Rect{}), PointerButton::middle);
    CHECK(f.browser.tab_count() == 1);
    click(sub.value_or(rummage::ui::Rect{}), PointerButton::middle);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_titles() == Names{"a", "sub"});
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.details().folder()->path() == f.a);  // still here

    // On a folder of the selection: every selected folder, files left out.
    f.browser.close_tab(1);
    fs::create_directories(f.a / "more");
    REQUIRE(run_until(f.loop, [&f] {
        const auto& entries = f.browser.details().folder()->entries();
        return std::ranges::find(entries, "more", &rummage::fs::Entry::name) != entries.end();
    }));
    REQUIRE(f.settle());
    draw();
    const std::shared_ptr<rummage::views::Folder> shown = f.browser.details().folder();
    shown->set_cursor(std::nullopt);
    for (const char* name : {"more", "sub", "one.txt"}) {
        const auto& entries = shown->entries();
        const auto it = std::ranges::find(entries, std::string{name}, &rummage::fs::Entry::name);
        REQUIRE(it != entries.end());
        shown->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                          rummage::views::SelectMode::toggle);
    }
    draw();
    const std::optional<rummage::ui::Rect> selected = row_of("sub");
    REQUIRE(selected);
    click(selected.value_or(rummage::ui::Rect{}), PointerButton::middle);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_titles() == Names{"a", "more", "sub"});
    CHECK(f.browser.active_tab() == 0);
}

TEST_CASE("zoom: the lists grow and shrink together, in every tab; the cursor's row stays") {
    Fixture f;
    const std::unique_ptr<rummage::ui::Offscreen> gl = offscreen();
    if (!gl) {
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1.0F, {});
        f.browser.draw(renderer, {0.0F, 0.0F, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    using rummage::views::kListSizes;
    using rummage::views::kUsualListSize;
    // Enough entries to scroll, the cursor far down.
    for (int i = 0; i < 60; ++i) {
        std::ofstream{f.a / std::format("file {:02}.txt", i)} << i;
    }
    REQUIRE(run_until(
        f.loop, [&f] { return f.browser.columns().focused_folder()->entries().size() == 62; }));
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    draw();
    CHECK(f.press(key(XKB_KEY_End)));  // the view follows the keyboard: the cursor in sight
    for (int i = 0; i < 30; ++i) {     // the cursor halfway down the view
        f.browser.key(key(XKB_KEY_Up), Browser::Clock::now());
    }
    const auto still = [&] {  // drawn until the scroll animation has arrived
        REQUIRE(run_until(f.loop, [&] {
            draw();
            return !f.browser.animating();
        }));
    };
    still();
    const auto anchor = [&f] { return f.browser.details().cursor_anchor(); };
    REQUIRE(anchor());
    const float before = anchor().value_or(std::pair{0.0F, 0.0F}).second;

    CHECK(f.press(key(XKB_KEY_plus, kCtrl)));  // larger
    still();
    CHECK(f.browser.details().list_size() == kUsualListSize + 1);
    CHECK(f.browser.columns().list_size() == kUsualListSize + 1);  // the columns too
    REQUIRE(anchor());
    // The cursor's row on the same height, give or take a pixel.
    CHECK(std::abs(anchor().value_or(std::pair{0.0F, 0.0F}).second - before) <= 8.0F);
    CHECK(f.browser.state().list_size == kUsualListSize + 1);

    // A new tab takes the size of the one it comes from; a zoom there is its own.
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.browser.details().list_size() == kUsualListSize + 1);
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = 500.0F, .y = 400.0F},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::scroll,
                       .x = 500.0F,
                       .y = 400.0F,
                       .wheel_y = -1.0F,
                       .modifiers = kCtrl},
                      Browser::Clock::now());  // Ctrl+wheel up
    CHECK(f.browser.details().list_size() == kUsualListSize + 2);
    f.browser.select_tab(0);
    CHECK(f.browser.details().list_size() == kUsualListSize + 1);  // as it was
    CHECK(f.browser.columns().list_size() == kUsualListSize + 1);

    // Ctrl+0 back to the usual size; Ctrl+minus never below the smallest.
    CHECK(f.press(key(XKB_KEY_0, kCtrl)));
    CHECK(f.browser.details().list_size() == kUsualListSize);
    for (std::size_t i = 0; i < kListSizes.size() + 2; ++i) {
        f.press(key(XKB_KEY_minus, kCtrl));
    }
    CHECK(f.browser.details().list_size() == 0);
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_plus, kCtrl)));  // in the columns too
    CHECK(f.browser.columns().list_size() == 1);
    CHECK(f.browser.columns().column(0).size() == kListSizes.at(1));
    CHECK(f.press(key(XKB_KEY_minus, kCtrl)));
    CHECK(f.browser.details().list_size() == 0);

    // The grid keeps its own size, a tab's too.
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(f.settle());
    const float grid = f.browser.grid().icon_size();
    CHECK(f.press(key(XKB_KEY_plus, kCtrl)));
    CHECK(f.browser.grid().icon_size() > grid);
    CHECK(f.browser.details().list_size() == 0);  // the lists untouched
    const auto zoomed = static_cast<std::int32_t>(f.browser.grid().icon_size());
    f.browser.select_tab(1);
    f.browser.set_mode(Browser::Mode::grid);
    CHECK(f.browser.grid().icon_size() == grid);  // the other tab's grid as it was
    CHECK(f.browser.details().list_size() == kUsualListSize + 2);

    // Each tab's zoom in the state, back as it was.
    const rummage::views::State state = f.browser.state();
    REQUIRE(state.tabs.size() == 2);
    CHECK(state.tabs[0].list_size == std::optional<std::size_t>{0});
    CHECK(state.tabs[0].grid_icon == std::optional<std::int32_t>{zoomed});
    CHECK(state.tabs[1].list_size == std::optional<std::size_t>{kUsualListSize + 2});
    CHECK(state.tabs[1].grid_icon == std::optional<std::int32_t>{static_cast<std::int32_t>(grid)});
    Browser again{f.loop,    f.pool,
                  f.watcher, rummage::fs::MimeDatabase::load(),
                  [] {},     [](CursorShape /*shape*/) {}};
    again.apply(state);
    REQUIRE(again.restore_tabs(state.tabs, state.shown_tab));
    REQUIRE(run_until(f.loop, [&again] { return !again.loading(); }));
    CHECK(again.details().list_size() == kUsualListSize + 2);
    again.select_tab(0);
    CHECK(again.details().list_size() == 0);
    CHECK(static_cast<std::int32_t>(again.grid().icon_size()) == zoomed);
}

TEST_CASE("folders: view, sort, zoom and widths follow navigation and survive restart") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_sort(rummage::fs::SortKey::modified, true);
    f.browser.set_grid_size(4);
    f.browser.set_list_size(3);
    f.browser.details().set_widths({80, 170, 240, 150, 150, 110, 110, 130, 220, 220});
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_sort(rummage::fs::SortKey::size, false);
    f.browser.set_grid_size(0);
    f.browser.set_list_size(0);
    f.browser.details().set_widths({90, 180, 250, 150, 150, 110, 110, 130, 220, 220});
    f.browser.back();
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::modified);
    CHECK(f.browser.sort_order().descending);
    CHECK(f.browser.grid().size_index() == 4);
    CHECK(f.browser.details().list_size() == 3);
    CHECK(f.browser.details().widths() ==
          rummage::views::DetailsWidths{80, 170, 240, 150, 150, 110, 110, 130, 220, 220});
    f.browser.forward();
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::details);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    CHECK_FALSE(f.browser.sort_order().descending);
    CHECK(f.browser.grid().size_index() == 0);
    CHECK(f.browser.details().list_size() == 0);
    CHECK(f.browser.details().widths() ==
          rummage::views::DetailsWidths{90, 180, 250, 150, 150, 110, 110, 130, 220, 220});
    const auto saved =
        rummage::views::parse_state(rummage::views::format_state(f.browser.state()), "state.toml");
    REQUIRE(saved.problems.empty());
    CHECK(saved.state.last_folder == f.b);
    Browser again{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    again.apply(saved.state);
    again.open(f.a);
    CHECK(again.mode() == Browser::Mode::grid);
    CHECK(again.grid().size_index() == 4);
    CHECK(again.sort_order().key == rummage::fs::SortKey::modified);
    again.open(f.b);
    CHECK(again.mode() == Browser::Mode::details);
    CHECK(again.details().list_size() == 0);
    CHECK(again.state().last_folder == f.b);
}

TEST_CASE("folders: closing a tab retains its settings and reveals preserve the folder left") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_grid_size(5);
    f.browser.open_tab(f.b, true);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.close_tab(1);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    CHECK(f.browser.mode() == Browser::Mode::details);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    f.browser.reveal(f.a, f.a / "one.txt");
    REQUIRE(f.settle());
    const auto saved = f.browser.state();  // completes the asynchronous location change
    REQUIRE(saved.folders.find(f.b));
    CHECK(saved.folders.find(f.b)->mode == rummage::views::State::Mode::details);
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.grid().size_index() == 5);
    CHECK(f.browser.grid().folder()->path() == f.a);
}

TEST_CASE("tabs: independent column choices in the same folder survive restart") {
    using Field = rummage::views::DetailsView::Field;
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().set_columns({Field::name, Field::owner, Field::permissions});
    f.browser.new_tab();
    REQUIRE(f.settle());
    CHECK(f.browser.details().columns() ==
          std::vector<Field>{Field::name, Field::owner, Field::permissions});
    f.browser.details().set_columns({Field::name, Field::created, Field::link_target});
    const auto state = f.browser.state();
    Browser again{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    again.apply(state);
    REQUIRE(again.restore_tabs(state.tabs, state.shown_tab));
    REQUIRE(run_until(f.loop, [&again] { return !again.loading(); }));
    CHECK(again.details().columns() ==
          std::vector<Field>{Field::name, Field::created, Field::link_target});
    again.select_tab(0);
    CHECK(again.details().columns() ==
          std::vector<Field>{Field::name, Field::owner, Field::permissions});
}

TEST_CASE("Show in Folder: grouped selection, first tab focus and properties") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_info_visible(false);
    f.browser.show_items({f.a / "one.txt", f.b / "two.txt", f.a / "sub/", f.a / "one.txt",
                          f.a / "gone", f.dir.path / "missing/item"},
                         true);
    REQUIRE(f.settle());
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.info_visible());
    CHECK(f.browser.info_pane().expanded());
    const auto first = f.browser.details().folder();
    REQUIRE(first);
    CHECK(first->path() == f.a);
    CHECK(first->selected_count() == 2);
    CHECK(first->is_selected("one.txt"));
    CHECK(first->is_selected("sub"));
    REQUIRE(first->cursor());
    CHECK(first->entries()[*first->cursor()].name == "one.txt");
    f.browser.select_tab(1);
    REQUIRE(f.settle());
    const auto second = f.browser.details().folder();
    REQUIRE(second);
    CHECK(second->path() == f.b);
    CHECK(second->selected_names() == Names{"two.txt"});
}

TEST_CASE("Show in Folder: active pane stays focused and hidden items are revealed") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    REQUIRE(f.browser.active_pane() == 1);
    std::ofstream{f.a / ".one"} << "1";
    std::ofstream{f.a / ".two"} << "2";
    f.browser.show_items({f.a / ".one", f.b / "two.txt", f.a / ".two"}, false);
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.shows_hidden());
    CHECK(f.browser.details().folder()->selected_names() == Names{".one", ".two"});
    f.browser.focus_pane(0);
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.details().folder()->path() == f.a);
    f.browser.focus_pane(1);
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.details().folder()->path() == f.a);
    f.browser.set_info_visible(false);
    f.browser.show_items({f.a / "gone", f.dir.path / "missing/item", "relative.txt"}, true);
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 0);
    CHECK_FALSE(f.browser.info_visible());
}

TEST_CASE("Show in Folder: filesystem root and folder opening preserve first focus") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.show_items({"/"}, false);
    REQUIRE(f.settle());
    CHECK(f.browser.details().folder()->path() == fs::path{"/"});
    f.browser.open(f.a);
    f.browser.open_tabs({f.b, f.a / "sub"});
    REQUIRE(f.settle());
    CHECK(f.browser.active_tab() == 0);
    CHECK(f.browser.tab_titles() == Names{"a", "b", "sub"});
}

TEST_CASE("Show in Folder: stale filters are cleared and selection remains visible in every view") {
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        f.browser.set_mode(mode);
        f.browser.set_name_filter("no-match");
        f.browser.show_items({f.a / "one.txt"}, false);
        REQUIRE(f.settle());
        CHECK(f.browser.name_filter().empty());
        const auto folder = mode == Browser::Mode::columns ? f.browser.columns().focused_folder()
                            : mode == Browser::Mode::grid  ? f.browser.grid().folder()
                                                           : f.browser.details().folder();
        REQUIRE(folder);
        REQUIRE(folder->single_selection());
        CHECK(folder->single_selection()->name == "one.txt");
        CHECK(folder->take_reveal() == rummage::views::Folder::Reveal::jump);
    }
}

TEST_CASE("views: selected labels stay within their rows and the status shows the full filename") {
    const auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    std::string name;
    SUBCASE("short filename") { name = "barrulus.png"; }
    SUBCASE("long filename") { name = std::string(170, 'a') + ".txt"; }
    std::ofstream{f.a / name} << "text";
    f.browser.set_info_visible(false);
    f.browser.set_sidebar_visible(false);
    f.browser.set_show_extensions(false);
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        f.browser.set_mode(mode);
        f.browser.show_items({f.a / name}, false);
        REQUIRE(f.settle());
        rummage::theme::Palette palette;
        palette.primary = rummage::ui::Color::rgb(0xff00ff);
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, 600, kHeight}, palette, icons, Browser::Clock::now());
        renderer.end_frame();
        const auto folder = mode == Browser::Mode::columns ? f.browser.columns().focused_folder()
                            : mode == Browser::Mode::grid  ? f.browser.grid().folder()
                                                           : f.browser.details().folder();
        REQUIRE(folder);
        CHECK(rummage::views::status_of(*folder) == name);
        const auto selected =
            mode == Browser::Mode::grid ? f.browser.grid().motion_bounds().selection
            : mode == Browser::Mode::details
                ? f.browser.details().motion_bounds().selection
                : f.browser.columns().column(f.browser.columns().focus()).motion_bounds().selection;
        REQUIRE_FALSE(selected.empty());
        std::vector<std::uint8_t> pixels(kWidth * kHeight * 4);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // No selected-name background may protrude above or below the regular row/cell.
        for (const int y :
             {static_cast<int>(selected.y) - 1, static_cast<int>(selected.bottom()) + 1}) {
            for (int x = static_cast<int>(selected.x) + 1;
                 x < static_cast<int>(selected.right()) - 1; ++x) {
                const auto offset = static_cast<std::size_t>(((kHeight - 1 - y) * kWidth + x) * 4);
                CHECK_FALSE((pixels[offset] == 255 && pixels[offset + 1] == 0 &&
                             pixels[offset + 2] == 255));
            }
        }
        if (mode != Browser::Mode::grid) {
            const float x = selected.x + 100;
            const float y = selected.bottom() + 6;
            if (mode == Browser::Mode::columns) {
                const auto& column = f.browser.columns().column(f.browser.columns().focus());
                CHECK(column.row_at(x, y) != folder->cursor());
            } else {
                const auto hit = f.browser.details().hit(x, y);
                REQUIRE(hit);
                CHECK(hit->entry != folder->cursor());
            }
        }
        const auto button = f.browser.sort_bounds();
        REQUIRE_FALSE(button.empty());
        PointerEvent event;
        event.kind = PointerEvent::Kind::press;
        event.button = PointerButton::left;
        event.x = button.x + 5;
        event.y = button.y + 5;
        f.browser.pointer(event, Browser::Clock::now());
        REQUIRE(f.browser.context_menu().is_open());
        const auto& items = f.browser.context_menu().items();
        const auto descending =
            std::ranges::find(items, "sort:descending", &rummage::views::MenuItem::id);
        REQUIRE(descending != items.end());
        descending->action();
        CHECK(f.browser.sort_order().descending);
        f.browser.context_menu().close();
    }
    CHECK_FALSE(f.browser.state().show_extensions);
}

TEST_CASE("views: View menu toggles hidden files and tracks Ctrl+H in every view") {
    Fixture f;
    std::ofstream{f.a / ".hidden"} << "hidden";
    f.browser.open(f.a);
    REQUIRE(f.settle());
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        f.browser.set_mode(mode);
        f.browser.open_view_menu(20, 20);
        const auto& items = f.visibility_items();
        auto hidden = std::ranges::find(items, "show_hidden", &rummage::views::MenuItem::id);
        REQUIRE(hidden != items.end());
        CHECK(hidden->checkable);
        CHECK_FALSE(hidden->checked);
        CHECK_FALSE(hidden->keys.empty());
        auto toggle = hidden->action;
        f.browser.context_menu().close();
        toggle();
        REQUIRE(f.settle());
        CHECK(f.browser.shows_hidden());
        const auto folder = mode == Browser::Mode::columns ? f.browser.columns().focused_folder()
                            : mode == Browser::Mode::grid  ? f.browser.grid().folder()
                                                           : f.browser.details().folder();
        CHECK(std::ranges::find(folder->entries(), ".hidden", &rummage::fs::Entry::name) !=
              folder->entries().end());
        f.browser.open_view_menu(20, 20);
        hidden =
            std::ranges::find(f.visibility_items(), "show_hidden", &rummage::views::MenuItem::id);
        REQUIRE(hidden != f.visibility_items().end());
        CHECK(hidden->checked);
        f.browser.context_menu().close();
        REQUIRE(f.press(key(XKB_KEY_h, kCtrl)));
        CHECK_FALSE(f.browser.shows_hidden());
        CHECK(std::ranges::find(folder->entries(), ".hidden", &rummage::fs::Entry::name) ==
              folder->entries().end());
    }
}

TEST_CASE("views: preview toggle replaces icons in every view and restores icons immediately") {
    const auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f{true};
        f.browser.set_motion_config({.enabled = false});
        fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "image/blocks.jpg", f.a / "photo.jpg");
        f.browser.set_info_visible(false);
        f.browser.set_sidebar_visible(false);
        f.browser.set_mode(mode);
        f.browser.open(f.a);
        REQUIRE(f.settle());
        CHECK(f.browser.shows_thumbnails());
        const auto toggle = [&] {
            f.browser.open_view_menu(20, 20);
            const auto& items = f.visibility_items();
            const auto item =
                std::ranges::find(items, "show_thumbnails", &rummage::views::MenuItem::id);
            REQUIRE(item != items.end());
            CHECK(item->label == "Thumbnails");
            CHECK(item->checkable);
            CHECK(item->checked == f.browser.shows_thumbnails());
            auto action = item->action;
            f.browser.context_menu().close();
            action();
        };
        const auto draw = [&] {
            renderer.begin_frame(kWidth, kHeight, 1, {});
            f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                           Browser::Clock::now());
            renderer.end_frame();
            std::vector<std::uint8_t> pixels(kWidth * kHeight * 4);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
        };
        toggle();
        CHECK_FALSE(f.browser.shows_thumbnails());
        draw();
        REQUIRE(f.settle());  // MIME sniffing is independent of thumbnails.
        const auto icons_only = draw();
        toggle();
        draw();
        REQUIRE(f.settle());
        const auto previews = draw();
        CHECK(previews != icons_only);
        // Only the thumbnail's icon square changes; names, folders and unsupported files stay put.
        std::size_t changed = 0;
        for (std::size_t pixel = 0; pixel < previews.size(); pixel += 4)
            if (!std::equal(previews.data() + pixel, previews.data() + pixel + 4,
                            icons_only.data() + pixel))
                ++changed;
        const float icon = mode == Browser::Mode::grid
                               ? f.browser.grid().icon_size()
                               : rummage::views::kListSizes[rummage::views::kUsualListSize].icon;
        CHECK(changed <= static_cast<std::size_t>((icon + 2) * (icon + 2)));
        toggle();
        CHECK(draw() == icons_only);
        CHECK_FALSE(f.browser.state().show_thumbnails);
        const auto saved = rummage::views::parse_state(
            rummage::views::format_state(f.browser.state()), "state.toml");
        REQUIRE(saved.problems.empty());
        CHECK_FALSE(saved.state.show_thumbnails);
        f.browser.set_show_thumbnails(true);
        f.browser.apply(saved.state);
        CHECK_FALSE(f.browser.shows_thumbnails());
    }
}

TEST_CASE("tabs: Ctrl+Shift+T brings back the tab closed last, where it was and as it was") {
    Fixture f;
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.open(f.a / "sub");
    REQUIRE(f.settle());
    f.browser.set_grid_size(1);
    const std::size_t steps = f.browser.history().size();
    const std::size_t here = f.browser.history_index();
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.browser.tab_count() == 3);
    f.browser.close_tab(1);  // the middle one, as its × does
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.closed_tab_count() == 1);

    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 3);
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.grid().folder()->path() == f.a / "sub");
    CHECK(f.browser.grid().size_index() == 1);
    CHECK(f.browser.history().size() == steps);
    CHECK(f.browser.history_index() == here);
    CHECK(f.browser.closed_tab_count() == 0);
    f.browser.back();
    REQUIRE(f.settle());
    CHECK(f.browser.grid().folder()->path() == f.b);
}

TEST_CASE("tabs: closed tabs come back newest first; ten are kept") {
    Fixture f;
    for (int i = 0; i < 11; ++i) {
        fs::create_directories(f.dir.path / std::format("d{}", i));
        f.browser.open_tab(f.dir.path / std::format("d{}", i), true);
        REQUIRE(f.settle());
    }
    CHECK(f.browser.tab_count() == 12);
    for (int i = 0; i < 11; ++i) {
        f.browser.close_tab(1);
    }
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.closed_tab_count() == Browser::kMaxClosedTabs);
    std::vector<std::string> back;
    for (std::size_t i = 0; i < Browser::kMaxClosedTabs; ++i) {
        CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
        REQUIRE(f.browser.columns().focused_folder() != nullptr);
        back.push_back(f.browser.columns().focused_folder()->path().filename().string());
    }
    CHECK(f.browser.tab_count() == 11);
    CHECK(back ==
          std::vector<std::string>{"d10", "d9", "d8", "d7", "d6", "d5", "d4", "d3", "d2", "d1"});
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 11);
    CHECK(f.browser.status_bar().message() == "No closed tab to reopen");
}

TEST_CASE("tabs: a closed tab whose folder is gone is dropped, with a message") {
    Fixture f;
    f.browser.open_tab(f.b, true);
    REQUIRE(f.settle());
    f.browser.close_tab(1);
    fs::remove_all(f.b);
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.status_bar().message() == "b no longer exists");
    CHECK(f.browser.closed_tab_count() == 0);

    // Two closed, the newer one gone: the next Ctrl+Shift+T takes the one before it.
    fs::create_directories(f.b);
    f.browser.open_tab(f.b, true);
    REQUIRE(f.settle());
    f.browser.open_tab(f.a / "sub", true);
    REQUIRE(f.settle());
    f.browser.close_tab(1);  // b
    f.browser.close_tab(1);  // sub
    fs::remove_all(f.a / "sub");
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.status_bar().message() == "sub no longer exists");
    CHECK(f.browser.closed_tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 2);
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    CHECK(f.browser.columns().focused_folder()->path() == f.b);
}

TEST_CASE("tabs: a tab closed by its × while another is shown keeps its last cursor") {
    Fixture f;
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.press(key(XKB_KEY_Down)));  // sub
    CHECK(f.press(key(XKB_KEY_Down)));  // one.txt
    REQUIRE(f.browser.columns().focused_folder()->cursor_entry() != nullptr);
    CHECK(f.browser.columns().focused_folder()->cursor_entry()->name == "one.txt");
    f.browser.select_tab(0);
    f.browser.close_tab(1);
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    CHECK(f.browser.columns().focused_folder()->path() == f.a);
    REQUIRE(f.browser.columns().focused_folder()->cursor_entry() != nullptr);
    CHECK(f.browser.columns().focused_folder()->cursor_entry()->name == "one.txt");
}

TEST_CASE("tabs: Ctrl+Shift+T in a name being edited is the field's") {
    Fixture f;
    f.browser.open_tab(f.b, true);
    REQUIRE(f.settle());
    f.browser.close_tab(1);
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    f.browser.key(key(XKB_KEY_T, kCtrlShift), Browser::Clock::now());
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.name_editor().active());
}

TEST_CASE("tabs: columns come back unfolded from their first folder") {
    Fixture f;
    fs::create_directories(f.a / "sub" / "deep");
    CHECK(f.press(key(XKB_KEY_t, kCtrl)));
    CHECK(f.press(key(XKB_KEY_Down)));   // sub
    CHECK(f.press(key(XKB_KEY_Right)));  // into sub
    CHECK(f.press(key(XKB_KEY_Down)));   // deep
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    const fs::path shown = f.browser.columns().focused_folder()->path();
    CHECK(f.press(key(XKB_KEY_w, kCtrl)));
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.columns().column(0).folder().path() == f.a);
    CHECK(f.browser.columns().focused_folder()->path() == shown);
}

TEST_CASE("tabs: a tab on the trash comes back to the trash") {
    Fixture f;
    f.browser.open_tab(rummage::fs::trash_path(), true);
    REQUIRE(f.settle());
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    CHECK(f.browser.columns().focused_folder()->is_trash());
    f.browser.close_tab(1);
    CHECK(f.browser.closed_tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 2);
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    CHECK(f.browser.columns().focused_folder()->is_trash());
    CHECK(f.browser.status_bar().message() != "trash:/// no longer exists");
    CHECK(f.browser.closed_tab_count() == 0);
}

TEST_CASE("tabs: a tab closed before its folder loaded comes back") {
    Fixture f;
    CHECK(f.browser.key(key(XKB_KEY_t, kCtrl), Browser::Clock::now()));
    CHECK(f.browser.key(key(XKB_KEY_w, kCtrl), Browser::Clock::now()));
    REQUIRE(f.settle());
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.browser.closed_tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_T, kCtrlShift)));
    CHECK(f.browser.tab_count() == 2);
    REQUIRE(f.browser.columns().focused_folder() != nullptr);
    CHECK(f.browser.columns().focused_folder()->path() == f.a);
}

TEST_CASE("tabs: reopening preserves filters and details settings independently in each pane") {
    Fixture f;
    f.browser.open_tab(f.b, true);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_name_filter("*.txt");
    f.browser.set_show_hidden(true, false);
    f.browser.set_list_size(2);
    const auto saved = f.browser.state().tabs[1];
    f.browser.close_tab(1);
    CHECK(f.browser.closed_tab_count() == 1);
    f.browser.toggle_split();
    f.browser.focus_pane(1);
    CHECK(f.browser.closed_tab_count() == 0);
    f.browser.reopen_tab();
    CHECK(f.browser.tab_count() == 1);
    f.browser.open_tab(f.a / "sub", true);
    REQUIRE(f.settle());
    f.browser.close_tab(1);
    CHECK(f.browser.closed_tab_count() == 1);
    f.browser.focus_pane(0);
    f.browser.reopen_tab();
    REQUIRE(f.settle());
    CHECK(f.browser.state().tabs[1] == saved);
    f.browser.focus_pane(1);
    CHECK(f.browser.closed_tab_count() == 1);
    f.browser.reopen_tab();
    REQUIRE(f.settle());
    CHECK(f.browser.picker_folder()->path() == f.a / "sub");
}

TEST_CASE("navigation: path-row buttons use the clicked pane history and cancel outside releases") {
    Fixture f;
    f.browser.set_sidebar_visible(false);
    const auto gl = offscreen();
    REQUIRE(gl);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
    };
    const auto point = [&](PointerEvent::Kind kind, float x, float y) {
        f.browser.pointer({.kind = kind, .x = x, .y = y, .button = PointerButton::left},
                          Browser::Clock::now());
    };
    const auto click = [&](float x, float y) {
        point(PointerEvent::Kind::motion, x, y);
        point(PointerEvent::Kind::press, x, y);
        point(PointerEvent::Kind::release, x, y);
        REQUIRE(f.settle());
        draw();
    };
    draw();
    CHECK(f.browser.cursor_at(19, 14) == CursorShape::standard);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    draw();
    CHECK(f.browser.cursor_at(19, 14) == CursorShape::pointer);
    point(PointerEvent::Kind::motion, 19, 14);
    point(PointerEvent::Kind::press, 19, 14);
    point(PointerEvent::Kind::release, 200, 100);
    CHECK(f.browser.picker_folder()->path() == f.b);
    click(19, 14);
    CHECK(f.browser.picker_folder()->path() == f.a);
    click(47, 14);
    CHECK(f.browser.picker_folder()->path() == f.b);
    f.browser.toggle_split();
    f.browser.focus_pane(1);
    f.browser.open(f.a / "sub");
    REQUIRE(f.settle());
    f.browser.open(f.a);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    draw();
    // Split panes add a 26px header and the tab row before the path.
    const auto right = f.browser.pane_bounds(1);
    click(right.x + 19, right.y + 26 + rummage::views::TabBar::kHeight + 14);
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.picker_folder()->path() == f.a / "sub");
    f.browser.focus_pane(0);
    CHECK(f.browser.picker_folder()->path() == f.b);
}

TEST_CASE("folders: reset restores built-in defaults for future visits and after restart") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_list_size(3);
    f.browser.set_grid_size(4);
    f.browser.details().set_column_visible(rummage::fs::SortKey::owner, true);
    REQUIRE(f.browser.has_custom_folder_view());
    f.browser.reset_folder_view();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.has_custom_folder_view());
    CHECK(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::name);
    CHECK_FALSE(f.browser.sort_order().descending);
    const auto reset = f.browser.state();
    REQUIRE(reset.folders.find(f.a));
    CHECK(*reset.folders.find(f.a) == rummage::views::State::FolderSettings{});
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_grid_size(5);
    f.browser.open(f.a);
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.has_custom_folder_view());
    const auto saved =
        rummage::views::parse_state(rummage::views::format_state(f.browser.state()), "state.toml");
    REQUIRE(saved.problems.empty());
    Browser again{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    again.apply(saved.state);
    again.open(f.a);
    REQUIRE(run_until(f.loop, [&] { return !again.loading(); }));
    CHECK_FALSE(again.has_custom_folder_view());
    again.open(f.b);
    REQUIRE(run_until(f.loop, [&] { return !again.loading(); }));
    CHECK(again.mode() == Browser::Mode::grid);
    CHECK(again.grid().size_index() == 5);
}
