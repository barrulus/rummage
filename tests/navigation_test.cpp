#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/fuzzy.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::ui::PointerButton;
using rummage::ui::PointerEvent;
using rummage::views::Browser;
using rummage::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

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

const Modifiers kAlt{.alt = true};
const Modifiers kCtrl{.ctrl = true};
const Modifiers kCtrlShift{.ctrl = true, .shift = true};

Names names_of(const Folder& folder) {
    Names names;
    for (const rummage::fs::Entry& entry : folder.entries()) {
        names.push_back(entry.name);
    }
    return names;
}

// root/{a/{x/, y.txt}, b/{z.txt}, .config/{n.conf}, .profile, c.txt}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(root / "a" / "x");
        fs::create_directories(root / "b");
        fs::create_directories(root / ".config");
        for (const fs::path& file :
             {root / "a" / "y.txt", root / "b" / "z.txt", root / ".config" / "n.conf",
              root / ".profile", root / "c.txt"}) {
            std::ofstream{file} << "x";
        }
        browser.set_home_trash(dir.path / "Trash");
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    // Presses event and waits for the folders it opened.
    void press(const KeyEvent& event) {
        CHECK(browser.key(event, Browser::Clock::now()));
        REQUIRE(settle());
    }

    void click(PointerButton button) {
        browser.pointer({.kind = PointerEvent::Kind::press,
                         .x = 400.0F,
                         .y = 300.0F,
                         .button = button,
                         .click_count = 1},
                        Browser::Clock::now());
        browser.pointer(
            {.kind = PointerEvent::Kind::release, .x = 400.0F, .y = 300.0F, .button = button},
            Browser::Clock::now());
        REQUIRE(settle());
    }

    std::shared_ptr<Folder> shown() {
        switch (browser.mode()) {
            case Browser::Mode::grid:
                return browser.grid().folder();
            case Browser::Mode::details:
                return browser.details().folder();
            case Browser::Mode::columns:
                break;
        }
        return browser.columns().focused_folder();
    }

    [[nodiscard]] fs::path where() { return shown()->path(); }

    [[nodiscard]] std::string cursor() {
        const rummage::fs::Entry* const entry = shown()->cursor_entry();
        return entry != nullptr ? entry->name : "";
    }

    void put_cursor(const std::string& name) {
        shown()->select_name(name);
        REQUIRE(run_until(loop, [&] { return cursor() == name; }));
    }
};

}  // namespace

TEST_CASE("keyboard browsing: Unicode prefix matching rejects invalid scalar sequences") {
    using rummage::views::filename_prefix_matches;
    CHECK(filename_prefix_matches("Éclair.txt", "éC"));
    CHECK(filename_prefix_matches("Αθήνα", "αθ"));
    CHECK(filename_prefix_matches("東京.txt", "東京"));
    CHECK_FALSE(filename_prefix_matches("éclair", "e"));
    CHECK_FALSE(filename_prefix_matches("ab", "abc"));
    for (const std::string invalid :
         {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "\xf8\x80\x80\x80"}) {
        CHECK_FALSE(filename_prefix_matches(invalid, invalid));
    }
}

TEST_CASE("keyboard browsing: type prefix edits and replaces selection in every view") {
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        std::ofstream{f.root / "Éclair.txt"} << "x";
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.put_cursor("b");
        f.press(KeyEvent{.keysym = XKB_KEY_C, .text = "C"});
        CHECK(f.cursor() == "c.txt");
        CHECK(f.shown()->selected_count() == 1);
        f.press(KeyEvent{.keysym = XKB_KEY_z, .text = "z"});
        CHECK(f.browser.type_jump_prefix() == "Cz");
        CHECK(f.cursor() == "c.txt");
        f.press(key(XKB_KEY_BackSpace));
        CHECK(f.browser.type_jump_prefix() == "C");
        CHECK(f.where() == f.root);
        f.press(key(XKB_KEY_Escape));
        CHECK(f.browser.type_jump_prefix().empty());
        f.press(KeyEvent{.keysym = XKB_KEY_eacute, .text = "é"});
        CHECK(f.cursor() == "Éclair.txt");
        f.press(key(XKB_KEY_BackSpace));
        CHECK(f.browser.type_jump_prefix().empty());
        CHECK(f.where() == f.root);
        f.press(KeyEvent{.keysym = XKB_KEY_period, .text = "."});
        CHECK(f.cursor() == "Éclair.txt");  // hidden entries are excluded
        f.press(key(XKB_KEY_Escape));
        f.browser.set_show_hidden(true);
        f.press(KeyEvent{.keysym = XKB_KEY_period, .text = "."});
        CHECK(f.cursor() == ".config");
        f.browser.open(f.root / "b");
        REQUIRE(f.settle());
        CHECK(f.browser.type_jump_prefix().empty());
        f.press(KeyEvent{.keysym = XKB_KEY_z, .text = "z"});
        CHECK(f.cursor() == "z.txt");
        REQUIRE(run_until(f.loop, [&] { return f.browser.type_jump_prefix().empty(); }));
        f.press(KeyEvent{.keysym = XKB_KEY_z, .text = "z"});
        CHECK(f.browser.type_jump_prefix() == "z");
    }
}

TEST_CASE("keyboard browsing: editors and configured commands win over typing and Vim") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_keyboard_config({.vim = true});
    REQUIRE(f.settle());
    f.put_cursor("a");
    f.press(KeyEvent{.keysym = XKB_KEY_j, .text = "j"});
    CHECK(f.cursor() == "b");
    CHECK(f.browser.type_jump_prefix().empty());
    f.press(key(XKB_KEY_k, kCtrl));
    CHECK(f.browser.command_palette().is_open());
    f.press(KeyEvent{.keysym = XKB_KEY_j, .text = "j"});
    CHECK(f.cursor() == "b");
    f.press(key(XKB_KEY_Escape));
    f.press(key(XKB_KEY_d, kAlt));
    REQUIRE(f.browser.breadcrumb().editing());
    f.press(KeyEvent{.keysym = XKB_KEY_j, .text = "j"});
    CHECK(f.browser.breadcrumb().text() == "j");
    CHECK(f.browser.type_jump_prefix().empty());
    f.press(key(XKB_KEY_Escape));
    f.put_cursor("c.txt");
    f.press(key(XKB_KEY_F2));
    REQUIRE(f.browser.name_editor().active());
    f.press(KeyEvent{.keysym = XKB_KEY_j, .text = "j"});
    CHECK(f.browser.name_editor().field().text() == "j.txt");
    CHECK(f.browser.type_jump_prefix().empty());
    f.press(key(XKB_KEY_Escape));
    f.browser.set_keyboard_config({.type_to_jump = false});
    CHECK_FALSE(f.browser.key(KeyEvent{.keysym = XKB_KEY_z, .text = "z"}, Browser::Clock::now()));
    CHECK(f.browser.type_jump_prefix().empty());
}

TEST_CASE("keyboard browsing: wrapping is opt in and preserves selection modifiers") {
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.put_cursor("c.txt");
        f.press(key(XKB_KEY_Down));
        CHECK(f.cursor() == "c.txt");
        f.browser.set_keyboard_config({.vim = true, .wrap = true});
        f.press(key(XKB_KEY_Page_Down));
        CHECK(f.cursor() == "c.txt");
        f.press(key(XKB_KEY_j, kCtrl));
        CHECK(f.cursor() == "a");
        CHECK(f.shown()->selected_names() == Names{"c.txt"});
        f.press(key(XKB_KEY_k));
        CHECK(f.cursor() == "c.txt");
        CHECK(f.shown()->selected_names() == Names{"c.txt"});
        f.browser.open(f.root / "b");
        REQUIRE(f.settle());
        f.put_cursor("z.txt");
        f.press(key(XKB_KEY_j));
        f.press(key(XKB_KEY_k));
        CHECK(f.cursor() == "z.txt");
        f.browser.open(f.root / "a" / "x");
        REQUIRE(f.settle());
        f.press(key(XKB_KEY_j));
        f.press(key(XKB_KEY_k));
        CHECK(f.cursor().empty());
    }
}

TEST_CASE("navigation: back and forward, the cursor where it was, in every view") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.put_cursor("a");
        f.press(key(XKB_KEY_Return));  // into a
        CHECK(f.where() == f.root / "a");
        f.put_cursor("y.txt");
        f.press(key(XKB_KEY_BackSpace));  // up: a step as well
        CHECK(f.where() == f.root);
        f.put_cursor("b");
        f.press(key(XKB_KEY_Return));
        CHECK(f.where() == f.root / "b");
        CHECK(f.browser.history().size() == 4);

        f.press(key(XKB_KEY_Left, kAlt));
        CHECK(f.where() == f.root);
        CHECK(f.cursor() == "b");
        f.press(key(XKB_KEY_Left, kAlt));
        CHECK(f.where() == f.root / "a");
        CHECK(f.cursor() == "y.txt");
        f.press(key(XKB_KEY_Left, kAlt));
        CHECK(f.where() == f.root);
        CHECK(f.cursor() == "a");
        f.press(key(XKB_KEY_Left, kAlt));  // nothing before
        CHECK(f.where() == f.root);
        CHECK(f.browser.history_index() == 0);

        f.press(key(XKB_KEY_Right, kAlt));
        CHECK(f.where() == f.root / "a");
        CHECK(f.cursor() == "y.txt");
        f.press(key(XKB_KEY_Right, kAlt));
        f.press(key(XKB_KEY_Right, kAlt));
        CHECK(f.where() == f.root / "b");
        f.press(key(XKB_KEY_Right, kAlt));  // nothing after
        CHECK(f.where() == f.root / "b");
        CHECK(f.browser.history().size() == 4);
        CHECK(f.browser.mode() == mode);
    }
}

TEST_CASE("navigation: the mouse's side buttons go back and forward") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(f.settle());
    f.browser.open(f.root / "b");  // as from the sidebar
    REQUIRE(f.settle());
    f.click(PointerButton::back);
    CHECK(f.where() == f.root);
    f.click(PointerButton::forward);
    CHECK(f.where() == f.root / "b");
}

TEST_CASE("navigation: opening a folder after going back drops the steps ahead") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.open(f.root / "a");
    REQUIRE(f.settle());
    f.press(key(XKB_KEY_Left, kAlt));
    f.browser.open(f.root / "b");
    REQUIRE(f.settle());
    CHECK(f.browser.history().size() == 2);
    CHECK(f.browser.history().back().folder == f.root / "b");
    f.press(key(XKB_KEY_Right, kAlt));
    CHECK(f.where() == f.root / "b");
}

TEST_CASE("navigation: a folder gone meanwhile is left out, with a message") {
    Fixture f;
    for (const fs::path& folder : {f.root, f.root / "a", f.root / "b"}) {
        f.browser.open(folder);
        f.browser.set_mode(Browser::Mode::details);
        REQUIRE(f.settle());
    }
    fs::remove_all(f.root / "a");
    f.press(key(XKB_KEY_Left, kAlt));
    CHECK(f.where() == f.root);
    CHECK(f.browser.status_bar().message() == "a no longer exists");
    CHECK(f.browser.history().size() == 2);

    // Nothing left that way: back where it was.
    fs::remove_all(f.root / "b");
    f.press(key(XKB_KEY_Right, kAlt));
    CHECK(f.where() == f.root);
    CHECK(f.browser.status_bar().message() == "b no longer exists");
    CHECK(f.browser.history().size() == 1);
}

TEST_CASE("navigation: the history keeps the last 100 steps") {
    Fixture f;
    for (int i = 0; i < 120; ++i) {
        f.browser.open(f.root / (i % 2 == 0 ? "a" : "b"));
    }
    REQUIRE(f.settle());
    CHECK(f.browser.history().size() == Browser::kHistorySize);
    CHECK(f.browser.history_index() == Browser::kHistorySize - 1);
}

TEST_CASE("hidden files: left out at first, Ctrl+H shows them in every view") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        CHECK_FALSE(f.browser.shows_hidden());
        CHECK(names_of(*f.shown()) == Names{"a", "b", "c.txt"});
        CHECK(rummage::views::status_of(*f.shown()) == "3 items, 2 hidden");

        f.press(key(XKB_KEY_h, kCtrl));
        CHECK(names_of(*f.shown()) == Names{".config", "a", "b", ".profile", "c.txt"});
        CHECK(rummage::views::status_of(*f.shown()) == "5 items");
        CHECK(f.browser.status_bar().message() == "Hidden files shown");

        // Whatever is selected while they are shown, nothing acts on them once left out.
        f.press(key(XKB_KEY_a, kCtrl));
        CHECK(f.shown()->selected_count() == 5);
        f.press(key(XKB_KEY_h, kCtrl));
        CHECK(names_of(*f.shown()) == Names{"a", "b", "c.txt"});
        CHECK(f.shown()->selected_count() == 3);
        CHECK(f.shown()->selected_names() == Names{"a", "b", "c.txt"});
    }
}

TEST_CASE("selection: Ctrl+Shift+I inverts it in every view, hidden files left out") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        CAPTURE(static_cast<int>(mode));
        Fixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.put_cursor("a");
        f.press(key(XKB_KEY_I, kCtrlShift));
        CHECK(f.shown()->selected_names() == Names{"b", "c.txt"});
        CHECK(f.cursor() == "a");
        f.press(key(XKB_KEY_i, kCtrlShift));  // with Caps Lock on
        CHECK(f.shown()->selected_names() == Names{"a"});
        CHECK(f.cursor() == "a");

        // With everything selected, inverting leaves nothing selected; the cursor stays.
        f.press(key(XKB_KEY_a, kCtrl));
        f.press(key(XKB_KEY_I, kCtrlShift));
        CHECK(f.shown()->selected_count() == 0);
        CHECK(f.cursor() == "a");
        CHECK(rummage::views::status_of(*f.shown()) == "3 items, 2 hidden");

        // Shown, the hidden files are inverted too; left out again, they leave the selection.
        f.press(key(XKB_KEY_h, kCtrl));
        f.press(key(XKB_KEY_I, kCtrlShift));
        CHECK(f.shown()->selected_count() == 5);
        f.press(key(XKB_KEY_h, kCtrl));
        CHECK(f.shown()->selected_names() == Names{"a", "b", "c.txt"});
    }
}

TEST_CASE("selection: Ctrl+Shift+I drops a paste on its way, the cursor with it") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.put_cursor("a");
    f.shown()->expect_names({"new.txt"});  // the cursor waits for it
    f.press(key(XKB_KEY_I, kCtrlShift));
    CHECK(f.shown()->selected_names() == Names{"a", "b", "c.txt"});
    CHECK(f.cursor() == "a");

    std::ofstream{f.root / "new.txt"} << "x";
    REQUIRE(run_until(f.loop, [&] { return f.shown()->entries().size() == 4; }));
    CHECK(f.shown()->selected_names() == Names{"a", "b", "c.txt"});
    CHECK(f.cursor() == "a");
}

TEST_CASE("hidden files: the cursor on one moves to the nearest entry shown") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.press(key(XKB_KEY_h, kCtrl));
    f.put_cursor(".profile");  // .config a b .profile c.txt: c.txt comes after it
    f.press(key(XKB_KEY_h, kCtrl));
    CHECK(f.cursor() == "c.txt");

    f.press(key(XKB_KEY_h, kCtrl));
    fs::remove(f.root / "c.txt");
    REQUIRE(run_until(f.loop, [&] { return f.shown()->entries().size() == 4; }));
    f.put_cursor(".profile");  // the last one: the entry before it
    f.press(key(XKB_KEY_h, kCtrl));
    CHECK(f.cursor() == "b");
}

TEST_CASE("hidden files: a hidden folder on the way to the one shown stays in sight") {
    Fixture f;
    f.browser.open(f.root / ".config");  // as from a bookmark in the sidebar
    REQUIRE(f.settle());
    f.press(key(XKB_KEY_BackSpace));  // the parent, the cursor on .config
    CHECK(f.where() == f.root);
    CHECK(f.cursor() == ".config");
    CHECK(names_of(*f.shown()) == Names{".config", "a", "b", "c.txt"});

    // Into it with the hidden files shown, then left out: the column before keeps the way.
    f.press(key(XKB_KEY_h, kCtrl));
    f.press(key(XKB_KEY_Right));
    CHECK(f.where() == f.root / ".config");
    f.press(key(XKB_KEY_h, kCtrl));
    CHECK(f.where() == f.root / ".config");
    CHECK(names_of(f.browser.columns().column(0).folder()) == Names{".config", "a", "b", "c.txt"});
    f.press(key(XKB_KEY_Left));
    CHECK(f.cursor() == ".config");
}

TEST_CASE("hidden files: a name changed to a hidden one keeps the cursor") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.put_cursor("c.txt");
    f.press(key(XKB_KEY_F2));
    REQUIRE(f.browser.name_editor().active());
    KeyEvent text;
    text.text = ".c";  // over "c", selected
    CHECK(f.browser.key(text, Browser::Clock::now()));
    f.press(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&] { return f.cursor() == ".c.txt"; }));
    CHECK(names_of(*f.shown()) == Names{"a", "b", ".c.txt"});
}
