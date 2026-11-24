#include "rummage/views/shortcuts.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
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
#include "rummage/views/shortcuts_overlay.hpp"
#include "rummage/views/status_bar.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::Command;
using rummage::views::Folder;
using rummage::views::Shortcut;
using rummage::views::shortcuts;
namespace scope = rummage::views::scope;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

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

bool cursor_name_is(const Folder& folder, std::string_view name) {
    const rummage::fs::Entry* entry = folder.cursor_entry();
    return entry != nullptr && entry->name == name;
}

KeyEvent chord(xkb_keysym_t keysym, Modifiers modifiers) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

std::string describe(const KeyEvent& event) {
    std::string out;
    out += event.modifiers.ctrl ? "Ctrl+" : "";
    out += event.modifiers.shift ? "Shift+" : "";
    out += event.modifiers.alt ? "Alt+" : "";
    std::array<char, 64> name{};
    xkb_keysym_get_name(event.keysym, name.data(), name.size());
    return out + name.data();
}

// root/{sub/{inner.txt}, a.txt, b.txt}
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
        make_files();
        browser.set_home_trash(dir.path / "Trash");  // Delete trashes them here, never in the home
        browser.set_places({{.label = "root", .path = root, .icon = "folder"},
                            {.label = "sub", .path = root / "sub", .icon = "folder"}});
    }

    // The files again, after Delete took some.
    void make_files() const {
        fs::create_directories(root / "sub");
        for (const char* name : {"sub/inner.txt", "a.txt", "b.txt"}) {
            if (!fs::exists(root / name)) {
                std::ofstream{root / name} << name;
            }
        }
    }

    std::shared_ptr<Folder> folder() {
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

    // The root in mode, loaded, the cursor on cursor_on, nothing over the view.
    void reset(Browser::Mode mode, const std::string& cursor_on = "sub") {
        // A new folder, say; an undone Delete may ask about a name made again meanwhile.
        REQUIRE(run_until(loop, [&] {
            if (browser.conflict_dialog().is_open()) {
                browser.cancel_transfer();
            }
            return !browser.loading();
        }));
        browser.breadcrumb().cancel_edit();
        browser.blur_filter();
        browser.set_name_filter({});
        browser.name_editor().cancel();
        REQUIRE_FALSE(browser.name_editor().active());
        browser.quick_look().close();
        browser.shortcuts_overlay().close();
        browser.context_menu().close();
        browser.command_palette().close();
        browser.confirm_dialog().close();
        browser.properties_dialog().close();
        browser.insights_view().close();
        browser.settings_dialog().close();
        browser.location_dialog().close();
        browser.sidebar().set_focused(false);
        make_files();
        browser.open(root);
        browser.set_mode(mode);
        REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
        folder()->select_name(cursor_on);
        REQUIRE(run_until(loop, [&] { return folder()->cursor_entry() != nullptr; }));
        REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    }
};

constexpr std::array<Browser::Mode, 3> kModes{Browser::Mode::columns, Browser::Mode::grid,
                                              Browser::Mode::details};

std::uint16_t scope_of(Browser::Mode mode) {
    switch (mode) {
        case Browser::Mode::grid:
            return scope::kGrid;
        case Browser::Mode::details:
            return scope::kDetails;
        case Browser::Mode::columns:
            break;
    }
    return scope::kColumns;
}

// Every chord a shortcut stands for.
std::vector<KeyEvent> chords_of(const Shortcut& shortcut) {
    std::vector<KeyEvent> out;
    for (const xkb_keysym_t keysym : shortcut.keysyms) {
        if (keysym != XKB_KEY_NoSymbol) {
            out.push_back(chord(keysym, shortcut.modifiers));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("keyboard focus leaves file contents visible in every view") {
    constexpr int width = 1000;
    constexpr int height = 700;
    rummage::ui::Offscreen gl{width, height};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    for (const auto mode : kModes) {
        Fixture f;
        f.reset(mode);
        f.browser.set_reduced_motion(true);
        const auto now = Browser::Clock::now() + 1s;
        const auto draw = [&] {
            renderer.begin_frame(width, height, 1, {});
            f.browser.draw(renderer, {0, 0, width, height}, rummage::theme::Palette{}, icons, now);
            renderer.end_frame();
            std::vector<std::uint8_t> pixels(width * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return pixels;
        };
        draw();  // Settle layout before comparing frames.
        const auto before = draw();
        REQUIRE(f.browser.key(chord(XKB_KEY_F6, {.shift = true}), now));
        const auto focused = draw();
        CHECK(before != focused);  // The focus indication is visible.
        bool contents_unchanged = true;
        for (int y = 150; y < 550; ++y) {
            for (int x = 300; x < 700; ++x) {
                const auto at = static_cast<std::size_t>((y * width + x) * 4);
                contents_unchanged &=
                    std::equal(before.data() + at, before.data() + at + 4, focused.data() + at);
            }
        }
        CHECK(contents_unchanged);
    }
}

TEST_CASE("shortcuts: the groups are the overlay's") {
    const std::vector<std::string_view> groups = rummage::views::overlay_groups();
    const std::set<std::string_view> expected{groups.begin(), groups.end()};
    for (const Shortcut& shortcut : shortcuts()) {
        CHECK_MESSAGE(expected.contains(shortcut.group), shortcut.group);
        CHECK(shortcut.scope != 0);
        // Commands may intentionally have no default binding; they remain bindable.
        if (chords_of(shortcut).empty() && shortcut.action != "Select a range" &&
            shortcut.action != "Move without selecting") {
            CHECK(shortcut.keys.empty());
            CHECK(shortcut.command != Command::none);
        }
    }
}

TEST_CASE("shortcuts: every one listed for a view works there") {
    Fixture f;
    for (const Browser::Mode mode : kModes) {
        for (const Shortcut& shortcut : shortcuts()) {
            if ((shortcut.scope & scope_of(mode)) == 0) {
                continue;
            }
            for (const KeyEvent& event : chords_of(shortcut)) {
                f.reset(mode);
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()),
                              describe(event) << " in mode " << static_cast<int>(mode));
            }
        }
    }
}

TEST_CASE("shortcuts: every one listed for the sidebar and Quick Look works there") {
    Fixture f;
    for (const Shortcut& shortcut : shortcuts()) {
        for (const KeyEvent& event : chords_of(shortcut)) {
            if ((shortcut.scope & scope::kSidebar) != 0) {
                f.reset(Browser::Mode::grid);
                f.browser.sidebar().set_focused(true);
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()),
                              describe(event) << " in the sidebar");
            }
            if ((shortcut.scope & scope::kQuickLook) != 0) {
                const bool markdown = event.keysym == XKB_KEY_m || event.keysym == XKB_KEY_M;
                if (markdown) {
                    std::ofstream{f.root / "preview.md"} << "# A Markdown preview\n";
                }
                f.reset(Browser::Mode::grid, markdown ? "preview.md" : "a.txt");
                if (markdown) {
                    f.browser.go_to(f.root, "preview.md");
                    REQUIRE(run_until(f.loop, [&] {
                        return cursor_name_is(*f.folder(), "preview.md") && !f.browser.loading();
                    }));
                    f.folder()->sniff(0, f.folder()->entries().size());
                    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
                }
                f.browser.key(chord(XKB_KEY_space, {}), Browser::Clock::now());
                REQUIRE(f.browser.quick_look().is_open());
                if (markdown) {
                    REQUIRE(run_until(f.loop, [&] { return !f.browser.quick_look().loading(); }));
                }
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()), describe(event)
                                                                               << " in Quick Look");
            }
        }
    }
}

TEST_CASE("shortcuts: every one listed for renaming works in the name's field") {
    Fixture f;
    for (const Shortcut& shortcut : shortcuts()) {
        if ((shortcut.scope & scope::kEditor) == 0) {
            continue;
        }
        for (const KeyEvent& event : chords_of(shortcut)) {
            f.reset(Browser::Mode::details, "a.txt");
            f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now());
            REQUIRE(f.browser.name_editor().active());
            CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()), describe(event));
        }
    }
}

TEST_CASE("shortcuts: nothing else does anything") {
    // The keys a file manager could plausibly react to, with each modifier.
    std::vector<xkb_keysym_t> keys{
        XKB_KEY_Up,        XKB_KEY_Down,   XKB_KEY_Left,       XKB_KEY_Right,  XKB_KEY_Page_Up,
        XKB_KEY_Page_Down, XKB_KEY_Home,   XKB_KEY_End,        XKB_KEY_Return, XKB_KEY_KP_Enter,
        XKB_KEY_BackSpace, XKB_KEY_Escape, XKB_KEY_space,      XKB_KEY_Tab,    XKB_KEY_Delete,
        XKB_KEY_Insert,    XKB_KEY_plus,   XKB_KEY_minus,      XKB_KEY_equal,  XKB_KEY_question,
        XKB_KEY_slash,     XKB_KEY_KP_Add, XKB_KEY_KP_Subtract};
    for (xkb_keysym_t key = XKB_KEY_a; key <= XKB_KEY_z; ++key) {
        keys.push_back(key);
    }
    for (xkb_keysym_t key = XKB_KEY_0; key <= XKB_KEY_9; ++key) {
        keys.push_back(key);
    }
    for (xkb_keysym_t key = XKB_KEY_F1; key <= XKB_KEY_F12; ++key) {
        keys.push_back(key);
    }
    const std::array<Modifiers, 5> modifiers{Modifiers{}, Modifiers{.ctrl = true},
                                             Modifiers{.shift = true}, Modifiers{.alt = true},
                                             Modifiers{.ctrl = true, .shift = true}};
    Fixture f;
    for (const Browser::Mode mode : kModes) {
        f.reset(mode);
        for (const xkb_keysym_t key : keys) {
            for (const Modifiers& mods : modifiers) {
                const KeyEvent event = chord(key, mods);
                if (!f.browser.key(event, Browser::Clock::now())) {
                    continue;
                }
                CHECK_MESSAGE((rummage::views::listed(event, scope_of(mode)) ||
                               rummage::views::command_for(event) != Command::none),
                              describe(event) << " in mode " << static_cast<int>(mode));
                f.reset(mode);
            }
        }
    }
    // Ctrl+Q is the window's: the browser leaves it alone.
    f.reset(Browser::Mode::grid);
    CHECK_FALSE(f.browser.key(chord(XKB_KEY_q, {.ctrl = true}), Browser::Clock::now()));
}

TEST_CASE("shortcuts: the overlay opens and closes, and takes every key meanwhile") {
    Fixture f;
    f.reset(Browser::Mode::details);
    const auto now = Browser::Clock::now();
    f.browser.key(chord(XKB_KEY_F1, {}), now);
    CHECK(f.browser.shortcuts_overlay().is_open());
    f.browser.shortcuts_overlay().layout({0, 0, 640, 480});
    CHECK(f.browser.key(chord(XKB_KEY_Right, {}), now));
    CHECK(f.browser.key(chord(XKB_KEY_Down, {}), now));
    CHECK(f.browser.shortcuts_overlay().scroll_offset() > 0.0F);
    CHECK(cursor_name_is(*f.folder(), "sub"));
    f.browser.key(chord(XKB_KEY_Escape, {}), now);
    CHECK_FALSE(f.browser.shortcuts_overlay().is_open());
    // "?" comes with Shift on most layouts.
    f.browser.key(chord(XKB_KEY_question, {.shift = true}), now);
    CHECK(f.browser.shortcuts_overlay().is_open());
    f.browser.key(chord(XKB_KEY_question, {.shift = true}), now);
    CHECK_FALSE(f.browser.shortcuts_overlay().is_open());
}

TEST_CASE("status bar: the status and the free space") {
    Fixture f;
    f.reset(Browser::Mode::details);
    std::shared_ptr<Folder> folder = f.folder();
    folder->set_cursor(std::nullopt);
    CHECK(rummage::views::status_of(*folder) == "3 items");
    folder->set_cursor(1);
    REQUIRE(folder->single_selection());
    CHECK(rummage::views::status_of(*folder) == folder->single_selection()->name);
    folder->set_cursor(0);
    REQUIRE(folder->single_selection());
    CHECK(rummage::views::status_of(*folder) == "sub");
    folder->set_cursor(1, rummage::views::SelectMode::keep);
    CHECK_FALSE(folder->single_selection());
    CHECK(rummage::views::status_of(*folder) == "sub");
    folder->set_cursor(std::nullopt);
    CHECK(rummage::views::status_of(*folder) == "3 items");
    folder->select_all();
    CHECK(rummage::views::status_of(*folder) == "3 of 3 selected");

    rummage::views::StatusBar& bar = f.browser.status_bar();
    bar.update(*folder);
    REQUIRE(run_until(f.loop, [&] { return !bar.loading(); }));
    REQUIRE(bar.free_space().has_value());
    CHECK(bar.free_space().value_or(rummage::views::FreeSpace{}).total > 0);
    bar.update(*folder);
    CHECK_FALSE(bar.loading());  // known for this listing

    // A file arrives: the listing changes, and the free space is read again.
    std::ofstream{f.root / "c.txt"} << "c";
    REQUIRE(run_until(f.loop, [&] { return folder->entries().size() == 4; }));
    bar.update(*folder);
    CHECK(bar.loading());
    CHECK(run_until(f.loop, [&] { return !bar.loading(); }));
}

namespace {
struct ResetBindings {
    ~ResetBindings() { (void)rummage::views::configure_shortcuts({}); }
};
}  // namespace

TEST_CASE("shortcuts: overrides replace defaults and reject conflicts atomically") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"rename", "Ctrl+r"}, {"trash", ""}}).empty());
    CHECK(command_for(chord(XKB_KEY_r, {.ctrl = true})) == Command::rename);
    CHECK(command_for(chord(XKB_KEY_R, {.ctrl = true})) == Command::rename);
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::none);
    CHECK(command_for(chord(XKB_KEY_Delete, {})) == Command::none);
    CHECK(rummage::views::keys_for(Command::rename) == "Ctrl+r");
    for (const auto& binding :
         std::vector<std::pair<std::string, std::string>>{{"rename", "Ctrl+c"},
                                                          {"rename", "Ctrl+q"},
                                                          {"rename", "Up"},
                                                          {"rename", "Ctrl+NoSuchKey"},
                                                          {"rename", "Ctrl+Ctrl+r"},
                                                          {"unknown", "F3"}}) {
        CHECK_FALSE(configure_shortcuts({binding}).empty());
        CHECK(command_for(chord(XKB_KEY_r, {.ctrl = true})) == Command::rename);
    }
    REQUIRE(configure_shortcuts({{"rename", "Ctrl+Shift+n"}, {"new_folder", "F2"}}).empty());
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::new_folder);
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::rename);
}

TEST_CASE("shortcuts: Alt+J, Alt+K and Alt+Page Up/Down scroll the info pane, and can be rebound") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    constexpr rummage::ui::Modifiers alt{.alt = true};
    CHECK(command_for(chord(XKB_KEY_k, alt)) == Command::scroll_preview_up);
    CHECK(command_for(chord(XKB_KEY_K, alt)) == Command::scroll_preview_up);
    CHECK(command_for(chord(XKB_KEY_j, alt)) == Command::scroll_preview_down);
    CHECK(command_for(chord(XKB_KEY_Page_Up, alt)) == Command::scroll_preview_page_up);
    CHECK(command_for(chord(XKB_KEY_Page_Down, alt)) == Command::scroll_preview_page_down);
    // Without Alt the keys are the view's own: Page Up and Page Down move the cursor.
    CHECK(command_for(chord(XKB_KEY_Page_Down, {})) == Command::none);
    CHECK(command_for(chord(XKB_KEY_j, {})) == Command::none);

    // The names and chords the example config shows.
    REQUIRE(configure_shortcuts({{"scroll_preview_up", "Alt+u"},
                                 {"scroll_preview_page_up", "Alt+Page_Up"},
                                 {"scroll_preview_page_down", "Ctrl+Alt+j"},
                                 {"scroll_preview_down", ""}})
                .empty());
    CHECK(command_for(chord(XKB_KEY_u, alt)) == Command::scroll_preview_up);
    CHECK(command_for(chord(XKB_KEY_Page_Up, alt)) == Command::scroll_preview_page_up);
    CHECK(command_for(chord(XKB_KEY_j, {.ctrl = true, .alt = true})) ==
          Command::scroll_preview_page_down);
    CHECK(command_for(chord(XKB_KEY_j, alt)) == Command::none);
    CHECK(command_for(chord(XKB_KEY_k, alt)) == Command::none);
}

TEST_CASE("shortcuts: custom selection and rename work in each view") {
    ResetBindings reset;
    REQUIRE(rummage::views::configure_shortcuts(
                {{"select_all", "Ctrl+g"}, {"toggle_selection", "Ctrl+j"}, {"rename", "Ctrl+r"}})
                .empty());
    Fixture f;
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        CHECK_FALSE(f.browser.key(chord(XKB_KEY_a, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.browser.key(chord(XKB_KEY_g, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.folder()->selected_count() == 3);
        CHECK(f.browser.key(chord(XKB_KEY_j, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.folder()->selected_count() == 2);
        CHECK(f.browser.key(chord(XKB_KEY_r, {.ctrl = true}), Browser::Clock::now()));
        REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
        REQUIRE(f.browser.batch_rename_dialog().is_open());
        CHECK_FALSE(f.browser.name_editor().active());
        CHECK(f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now()));
        CHECK_FALSE(f.browser.batch_rename_dialog().is_open());

        // The same configured shortcut still edits a single entry in place.
        f.folder()->select_name("a.txt");
        CHECK(f.browser.key(chord(XKB_KEY_r, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.browser.name_editor().active());
        CHECK(f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now()));
        CHECK_FALSE(f.browser.name_editor().active());
    }
}

TEST_CASE("shortcuts: tab aliases and combined help rows follow overrides") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"previous_tab", "Ctrl+Shift+Tab"}, {"back", "Alt+b"}}).empty());
    CHECK(command_for(chord(XKB_KEY_ISO_Left_Tab, {.ctrl = true, .shift = true})) ==
          Command::previous_tab);
    CHECK(rummage::views::keys_for(Command::back) == "Alt+b");
    CHECK(rummage::views::keys_for(Command::forward) == "Alt+Right");
    CHECK(rummage::views::keys_for(Command::next_tab) == "Ctrl+Tab");
}

TEST_CASE("shortcuts: overflow is reachable by wheel, keyboard and scrollbar") {
    rummage::views::ShortcutsOverlay overlay{[] {}};
    overlay.open();
    overlay.layout({0, 0, 640, 480});
    overlay.key(chord(XKB_KEY_Right, {}));
    REQUIRE(overlay.max_scroll() > 0.0F);
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::scroll, .wheel_y = 1.0F});
    CHECK(overlay.scroll_offset() == 48.0F);
    overlay.key(chord(XKB_KEY_End, {}));
    CHECK(overlay.scroll_offset() == overlay.max_scroll());
    overlay.key(chord(XKB_KEY_Page_Up, {}));
    CHECK(overlay.scroll_offset() < overlay.max_scroll());
    overlay.key(chord(XKB_KEY_Home, {}));
    CHECK(overlay.scroll_offset() == 0.0F);
    // Content clicks leave the reference open, while the scrollbar can be dragged to the end.
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 100,
                     .y = 180,
                     .button = rummage::ui::PointerButton::left});
    CHECK(overlay.is_open());
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 576,
                     .y = 128,
                     .button = rummage::ui::PointerButton::left});
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 576, .y = 440});
    CHECK(overlay.scroll_offset() == overlay.max_scroll());
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::release});
    overlay.layout({0, 0, 1600, 4000});
    overlay.key(chord(XKB_KEY_Left, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    CHECK(overlay.scroll_offset() == 0.0F);
    overlay.layout({0, 0, 640, 480});
    overlay.key(chord(XKB_KEY_End, {}));
    overlay.close();
    overlay.open();
    CHECK(overlay.scroll_offset() == 0.0F);
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 0,
                     .y = 0,
                     .button = rummage::ui::PointerButton::left});
    CHECK_FALSE(overlay.is_open());
}

TEST_CASE("shortcuts: groups start collapsed and toggle by mouse and keyboard") {
    rummage::views::ShortcutsOverlay overlay{[] {}};
    overlay.open();
    overlay.layout({0, 0, 1000, 800});
    CHECK(overlay.max_scroll() == 0.0F);
    // General is the first header; clicking either its title or row toggles it.
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 100,
                     .y = 140,
                     .button = rummage::ui::PointerButton::left});
    REQUIRE(overlay.max_scroll() > 0.0F);
    overlay.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = 500,
                     .y = 140,
                     .button = rummage::ui::PointerButton::left});
    CHECK(overlay.max_scroll() == 0.0F);
    overlay.key(chord(XKB_KEY_Tab, {}));
    overlay.key(chord(XKB_KEY_Return, {}));
    CHECK(overlay.max_scroll() > 0.0F);
    overlay.key(chord(XKB_KEY_space, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    // Shift+Tab wraps to Quick Look; tabbing forward returns to General.
    overlay.key(chord(XKB_KEY_ISO_Left_Tab, {.shift = true}));
    overlay.key(chord(XKB_KEY_Right, {}));
    overlay.key(chord(XKB_KEY_Tab, {}));
    overlay.key(chord(XKB_KEY_Right, {}));
    CHECK(overlay.max_scroll() > 0.0F);
    overlay.key(chord(XKB_KEY_Left, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    CHECK(overlay.is_open());
}

TEST_CASE("actions: shortcuts validate registry IDs and preserve bindings after errors") {
    ResetBindings reset;
    using rummage::views::action_for;
    using rummage::views::configure_shortcuts;
    const std::vector<rummage::core::Action> actions{
        {.id = "inspect", .label = "Inspect", .argv = {"inspector", "{file}"}, .mime = {}}};
    REQUIRE(configure_shortcuts(
                {{"action:inspect", "Ctrl+Alt+i"}, {"copy_location", "Ctrl+Shift+c"}}, actions)
                .empty());
    CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})) == "inspect");
    CHECK(action_for(chord(XKB_KEY_C, {.ctrl = true, .shift = true})) == "copy_location");
    CHECK(rummage::views::keys_for_action("inspect") == "Ctrl+Alt+i");
    for (const auto& binding :
         std::vector<std::pair<std::string, std::string>>{{"action:missing", "F3"},
                                                          {"action:inspect", "Ctrl+c"},
                                                          {"action:inspect", "Up"},
                                                          {"action:inspect", "Ctrl+q"}}) {
        CHECK_FALSE(configure_shortcuts({binding}, actions).empty());
        CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})) == "inspect");
    }
    CHECK_FALSE(
        configure_shortcuts({{"copy_location", "F3"}, {"action:copy_location", "F5"}}, actions)
            .empty());
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})).empty());
}

TEST_CASE("actions: copy name has a configurable default without changing file copy or location") {
    ResetBindings reset;
    using rummage::views::action_for;
    using rummage::views::configure_shortcuts;
    const auto copy_name = chord(XKB_KEY_c, {.ctrl = true, .alt = true});
    CHECK(action_for(copy_name) == "copy_name");
    CHECK(rummage::views::keys_for_action("copy_name") == "Ctrl+Alt+C");
    CHECK(rummage::views::command_for(chord(XKB_KEY_c, {.ctrl = true})) == Command::copy);
    REQUIRE(configure_shortcuts({{"copy_location", "Ctrl+Shift+c"}}).empty());
    CHECK(action_for(copy_name) == "copy_name");
    CHECK(action_for(chord(XKB_KEY_C, {.ctrl = true, .shift = true})) == "copy_location");
    REQUIRE(configure_shortcuts({{"action:copy_name", "Ctrl+Alt+F3"}}).empty());
    CHECK(action_for(copy_name).empty());
    CHECK(action_for(chord(XKB_KEY_F3, {.ctrl = true, .alt = true})) == "copy_name");
    CHECK_FALSE(
        configure_shortcuts({{"copy_name", "Ctrl+Alt+F3"}, {"action:copy_name", "Ctrl+Alt+F5"}})
            .empty());
    REQUIRE(configure_shortcuts({{"copy_name", ""}}).empty());
    CHECK(action_for(copy_name).empty());
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(action_for(copy_name) == "copy_name");
}

TEST_CASE("actions: copy name shortcut works in every view and respects name editors") {
    ResetBindings reset;
    Fixture f;
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    const auto event = chord(XKB_KEY_C, {.ctrl = true, .alt = true});
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        CHECK(f.browser.key(event, Browser::Clock::now()));
        REQUIRE_FALSE(offered.empty());
        CHECK(offered.front().second == "a.txt");
        offered.clear();
        f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now());
        REQUIRE(f.browser.name_editor().active());
        f.browser.key(event, Browser::Clock::now());
        CHECK(offered.empty());
    }
}

TEST_CASE("actions: external shortcuts use the same targets in all views and respect editors") {
    ResetBindings reset;
    const std::vector<rummage::core::Action> actions{
        {.id = "inspect", .label = "Inspect", .argv = {"inspector", "{file}"}, .mime = {}}};
    REQUIRE(
        rummage::views::configure_shortcuts({{"action:inspect", "Ctrl+Alt+i"}}, actions).empty());
    Fixture f;
    f.browser.set_actions(actions);
    std::vector<std::string> calls;
    f.browser.set_action_handoff(
        [&](const auto&, const auto& context) {
            REQUIRE(context.targets.size() == 1);
            calls.push_back(context.targets.front().path.string());
        },
        [](const auto&, const auto&) { return true; });
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        const auto event = chord(XKB_KEY_i, {.ctrl = true, .alt = true});
        CHECK(f.browser.key(event, Browser::Clock::now()));
        REQUIRE_FALSE(calls.empty());
        CHECK(calls.back() == (f.root / "a.txt").string());
        const auto count = calls.size();
        f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now());
        REQUIRE(f.browser.name_editor().active());
        f.browser.key(event, Browser::Clock::now());
        CHECK(calls.size() == count);
        f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now());
    }
}

TEST_CASE("shortcuts: pane commands without defaults can be bound and disabled") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"swap_panes", "Ctrl+Shift+s"},
                                 {"orient_panes", "Ctrl+Shift+F3"},
                                 {"open_other_pane", "Ctrl+Shift+o"},
                                 {"same_other_pane", "Ctrl+Shift+d"}})
                .empty());
    CHECK(command_for(chord(XKB_KEY_s, {.ctrl = true, .shift = true})) == Command::swap_panes);
    CHECK(command_for(chord(XKB_KEY_F3, {.ctrl = true, .shift = true})) == Command::orient_panes);
    CHECK(command_for(chord(XKB_KEY_o, {.ctrl = true, .shift = true})) == Command::open_other_pane);
    CHECK(command_for(chord(XKB_KEY_d, {.ctrl = true, .shift = true})) == Command::same_other_pane);
    REQUIRE(configure_shortcuts({{"toggle_split", ""}, {"switch_pane", ""}}).empty());
    CHECK(command_for(chord(XKB_KEY_F3, {})) == Command::none);
    CHECK(command_for(chord(XKB_KEY_Tab, {})) == Command::none);
}

TEST_CASE("shortcuts: folder jumps validate, reload and navigate within the current tab") {
    ResetBindings reset;
    Fixture f;
    f.reset(Browser::Mode::columns);
    using rummage::views::configure_shortcuts;
    const auto destination = f.root / "sub";
    const auto binding = "go:" + destination.string();
    const auto event = chord(XKB_KEY_j, {.ctrl = true, .alt = true});
    REQUIRE(configure_shortcuts({{binding, "Ctrl+Alt+j"}}).empty());
    CHECK(rummage::views::folder_for(event) == destination.string());
    CHECK(rummage::views::action_for(event).empty());
    const auto groups = rummage::views::overlay_groups();
    for (const auto& shortcut : shortcuts()) {
        if (!shortcut.folder_path.empty()) {
            CHECK(std::find(groups.begin(), groups.end(), shortcut.group) != groups.end());
        }
    }
    CHECK_FALSE(configure_shortcuts({{"go:relative", "Ctrl+Alt+j"}}).empty());
    CHECK_FALSE(configure_shortcuts({{binding, "Ctrl+c"}}).empty());
    CHECK_FALSE(configure_shortcuts({{binding, "Ctrl+q"}}).empty());
    CHECK(rummage::views::folder_for(event) == destination.string());
    f.browser.sidebar().set_focused(true);
    CHECK(f.browser.key(event, Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == destination);
    CHECK(f.browser.tab_count() == 1);
    CHECK_FALSE(f.browser.sidebar().focused());
    f.browser.back();
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == f.root);
    REQUIRE(configure_shortcuts({{"go:" + (f.root / "missing").string(), "Ctrl+Alt+j"}}).empty());
    CHECK(f.browser.key(event, Browser::Clock::now()));
    CHECK(f.folder()->path() == f.root);
    CHECK(f.browser.status_bar().message().starts_with("Folder unavailable:"));
    REQUIRE(configure_shortcuts({{binding, ""}}).empty());
    CHECK(rummage::views::folder_for(event).empty());
}

TEST_CASE("shortcuts: stopping a transfer is configurable, disableable, and listed in settings") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    using rummage::views::keys_for;
    CHECK(command_for(chord(XKB_KEY_Escape, {.shift = true})) == Command::stop_transfer);
    CHECK(command_for(chord(XKB_KEY_Escape, {})) == Command::none);
    CHECK(keys_for(Command::stop_transfer) == "Shift+Escape");
    const auto settings = rummage::views::binding_settings();
    const auto stop = std::ranges::find(settings, "stop_transfer",
                                        [](const auto& binding) { return binding.first; });
    REQUIRE(stop != settings.end());
    CHECK(stop->second == "Stop the copy, move or delete · Default: Shift+Escape");
    REQUIRE(configure_shortcuts({{"stop_transfer", "Ctrl+Alt+s"}}).empty());
    CHECK(command_for(chord(XKB_KEY_Escape, {.shift = true})) == Command::none);
    CHECK(command_for(chord(XKB_KEY_s, {.ctrl = true, .alt = true})) == Command::stop_transfer);
    CHECK(keys_for(Command::stop_transfer) == "Ctrl+Alt+s");
    REQUIRE(configure_shortcuts({{"stop_transfer", ""}}).empty());
    CHECK(command_for(chord(XKB_KEY_Escape, {.shift = true})) == Command::none);
    CHECK(keys_for(Command::stop_transfer).empty());
}

TEST_CASE("copy path: default chord yields to existing custom bindings") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    const auto chord_copy = chord(XKB_KEY_c, {.ctrl = true, .shift = true});
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(rummage::views::command_for(chord_copy) == Command::copy_path);
    REQUIRE(configure_shortcuts({{"copy_location", "Ctrl+Shift+C"}}).empty());
    CHECK(rummage::views::action_for(chord_copy) == "copy_location");
    CHECK(rummage::views::keys_for(Command::copy_path).empty());
    CHECK_FALSE(
        configure_shortcuts({{"copy_path", "Ctrl+Shift+C"}, {"copy_location", "Ctrl+Shift+C"}})
            .empty());
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(rummage::views::command_for(chord_copy) == Command::copy_path);
}

TEST_CASE("shortcuts: thumbnail and extension bindings toggle every view and update Show hints") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(rummage::views::keys_for(Command::toggle_thumbnails).empty());
    CHECK(rummage::views::keys_for(Command::toggle_extensions).empty());
    const auto settings = rummage::views::binding_settings();
    for (const auto* id : {"toggle_thumbnails", "toggle_extensions"})
        CHECK(std::ranges::any_of(settings, [id](const auto& item) { return item.first == id; }));
    REQUIRE(configure_shortcuts(
                {{"toggle_thumbnails", "Ctrl+Alt+t"}, {"toggle_extensions", "Ctrl+Alt+e"}})
                .empty());
    Fixture f;
    for (const auto mode : kModes) {
        f.reset(mode);
        f.browser.set_show_thumbnails(true);
        f.browser.set_show_extensions(true);
        for (int repeat = 0; repeat < 2; ++repeat) {
            CHECK(f.browser.key(chord(XKB_KEY_t, {.ctrl = true, .alt = true}),
                                Browser::Clock::now()));
            CHECK(f.browser.shows_thumbnails() == (repeat == 1));
            CHECK(f.browser.key(chord(XKB_KEY_e, {.ctrl = true, .alt = true}),
                                Browser::Clock::now()));
            CHECK(f.browser.shows_extensions() == (repeat == 1));
        }
        f.browser.open_view_menu(0, 0);
        const auto& items = f.browser.context_menu().items();
        const auto show = std::ranges::find(items, "show", &rummage::views::MenuItem::id);
        REQUIRE(show != items.end());
        for (const auto& [id, keys] : {std::pair{"show_thumbnails", "Ctrl+Alt+t"},
                                       std::pair{"show_extensions", "Ctrl+Alt+e"}}) {
            const auto item = std::ranges::find(show->children, id, &rummage::views::MenuItem::id);
            REQUIRE(item != show->children.end());
            CHECK(item->checked);
            CHECK(item->keys == keys);
        }
        f.browser.context_menu().close();
    }
    REQUIRE(configure_shortcuts({{"toggle_thumbnails", ""}, {"toggle_extensions", ""}}).empty());
    CHECK(rummage::views::command_for(chord(XKB_KEY_t, {.ctrl = true, .alt = true})) ==
          Command::none);
    CHECK(rummage::views::command_for(chord(XKB_KEY_e, {.ctrl = true, .alt = true})) ==
          Command::none);
}

TEST_CASE("shortcuts: reopen and inversion respect custom bindings and existing assignments") {
    ResetBindings reset;
    using rummage::views::command_for;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(command_for(chord(XKB_KEY_t, {.ctrl = true, .shift = true})) == Command::reopen_tab);
    REQUIRE(configure_shortcuts({{"reopen_tab", "Ctrl+Alt+t"}, {"invert_selection", "Ctrl+Alt+i"}})
                .empty());
    CHECK(command_for(chord(XKB_KEY_t, {.ctrl = true, .alt = true})) == Command::reopen_tab);
    Fixture f;
    f.reset(Browser::Mode::details);
    f.browser.set_name_filter("*.txt");
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    f.folder()->select_name("a.txt");
    f.browser.key(chord(XKB_KEY_i, {.ctrl = true, .alt = true}), Browser::Clock::now());
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"b.txt"});
    REQUIRE(configure_shortcuts({{"invert_selection", ""}, {"reopen_tab", ""}}).empty());
    f.browser.key(chord(XKB_KEY_i, {.ctrl = true, .shift = true}), Browser::Clock::now());
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"b.txt"});
    CHECK(command_for(chord(XKB_KEY_t, {.ctrl = true, .shift = true})) == Command::none);
    REQUIRE(configure_shortcuts({{"new_tab", "Ctrl+Shift+t"}, {"toggle_info", "Ctrl+Shift+i"}})
                .empty());
    CHECK(command_for(chord(XKB_KEY_t, {.ctrl = true, .shift = true})) == Command::new_tab);
    CHECK(command_for(chord(XKB_KEY_i, {.ctrl = true, .shift = true})) == Command::toggle_info);
}

TEST_CASE("shortcuts: pattern selection and folder reset have configurable bindings") {
    using namespace rummage::views;
    REQUIRE(
        configure_shortcuts({{"select_pattern", "Ctrl+Alt+p"}, {"reset_folder_view", "Ctrl+Alt+r"}})
            .empty());
    CHECK(command_for(chord(XKB_KEY_p, {.ctrl = true, .alt = true})) == Command::select_pattern);
    CHECK(command_for(chord(XKB_KEY_r, {.ctrl = true, .alt = true})) == Command::reset_folder_view);
    CHECK(keys_for(Command::select_pattern) == "Ctrl+Alt+p");
    REQUIRE(configure_shortcuts({}).empty());
}

TEST_CASE("keyboard browsing: edit path without drawing, cancel and return to the files") {
    ResetBindings reset;
    Fixture f;
    f.reset(Browser::Mode::details, "a.txt");
    const auto press = [&](xkb_keysym_t symbol, Modifiers mods = {}) {
        CHECK(f.browser.key(chord(symbol, mods), Browser::Clock::now()));
    };
    press(XKB_KEY_F6);
    REQUIRE(f.browser.sidebar().focused());
    press(XKB_KEY_d, {.alt = true});
    REQUIRE(f.browser.breadcrumb().editing());
    CHECK_FALSE(f.browser.sidebar().focused());
    CHECK(f.browser.breadcrumb().text() == f.root.string());
    f.browser.breadcrumb().paste((f.root / "sub").string());
    press(XKB_KEY_Return);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK_FALSE(f.browser.breadcrumb().editing());
    CHECK(f.folder()->path() == f.root / "sub");
    press(XKB_KEY_d, {.alt = true});
    f.browser.breadcrumb().paste((f.root / "missing").string());
    press(XKB_KEY_Return);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.breadcrumb().editing());
    CHECK_FALSE(f.browser.breadcrumb().error().empty());
    press(XKB_KEY_F6, {.shift = true});
    CHECK_FALSE(f.browser.breadcrumb().editing());
    CHECK(f.folder()->path() == f.root / "sub");
    press(XKB_KEY_Home);
    CHECK(cursor_name_is(*f.folder(), "inner.txt"));
    press(XKB_KEY_f, {.ctrl = true, .shift = true});
    REQUIRE(f.browser.filtering());
    press(XKB_KEY_F6, {.shift = true});
    CHECK_FALSE(f.browser.filtering());
}

TEST_CASE("keyboard browsing: Places actions and context menu target the focused place") {
    Fixture f;
    f.reset(Browser::Mode::details, "a.txt");
    const auto press = [&](xkb_keysym_t symbol, Modifiers mods = {}) {
        CHECK(f.browser.key(chord(symbol, mods), Browser::Clock::now()));
    };
    press(XKB_KEY_F6);
    press(XKB_KEY_End);
    REQUIRE(f.browser.sidebar().cursor_place());
    CHECK(f.browser.sidebar().cursor_place()->path == f.root / "sub");
    const auto selection = f.folder()->selected_names();
    press(XKB_KEY_F10, {.shift = true});
    REQUIRE(f.browser.context_menu().is_open());
    const auto& items = f.browser.context_menu().items();
    CHECK(std::ranges::find(items, "open_tab", &rummage::views::MenuItem::id) != items.end());
    press(XKB_KEY_Escape);
    CHECK(f.browser.sidebar().focused());
    CHECK(f.folder()->selected_names() == selection);
    fs::path terminal;
    f.browser.set_terminal_handler([&](const auto& path) { terminal = path; });
    press(XKB_KEY_F4);
    CHECK(terminal == f.root / "sub");
    // Reopening a keyboard menu starts on its first enabled item (Open), not a mouse hover.
    press(XKB_KEY_Menu);
    press(XKB_KEY_Down);
    press(XKB_KEY_Return);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.state().tabs.size() == 2);
    CHECK(f.folder()->path() == f.root / "sub");
    CHECK_FALSE(f.browser.sidebar().focused());
    f.reset(Browser::Mode::details, "a.txt");
    press(XKB_KEY_F6);
    press(XKB_KEY_End);
    press(XKB_KEY_Return, {.ctrl = true, .alt = true});
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.split_visible());
    CHECK(f.folder()->path() == f.root / "sub");
    CHECK_FALSE(f.browser.sidebar().focused());
}

TEST_CASE("keyboard browsing: remapped movement preserves selection and editing contexts") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts(
                {{"move_down", "j"}, {"move_up", "k"}, {"command_palette", "Ctrl+Shift+p"}})
                .empty());
    Fixture f;
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        CHECK(f.browser.key(chord(XKB_KEY_J, {.shift = true}), Browser::Clock::now()));
        CHECK(cursor_name_is(*f.folder(), "b.txt"));
        CHECK(f.folder()->selected_names() == std::vector<std::string>{"a.txt", "b.txt"});
        CHECK(f.browser.key(chord(XKB_KEY_k, {.ctrl = true}), Browser::Clock::now()));
        CHECK(cursor_name_is(*f.folder(), "a.txt"));
        CHECK(f.folder()->selected_names() == std::vector<std::string>{"a.txt", "b.txt"});
        CHECK_FALSE(f.browser.key(chord(XKB_KEY_Down, {}), Browser::Clock::now()));
        CHECK(cursor_name_is(*f.folder(), "a.txt"));
        CHECK(f.browser.key(chord(XKB_KEY_F6, {}), Browser::Clock::now()));
        CHECK(f.browser.key(chord(XKB_KEY_j, {}), Browser::Clock::now()));
        REQUIRE(f.browser.sidebar().cursor_place());
        CHECK(f.browser.sidebar().cursor_place()->path == f.root / "sub");
        CHECK(f.browser.key(chord(XKB_KEY_F6, {.shift = true}), Browser::Clock::now()));
        CHECK(f.browser.key(chord(XKB_KEY_d, {.alt = true}), Browser::Clock::now()));
        KeyEvent typed = chord(XKB_KEY_j, {});
        typed.text = "j";
        CHECK(f.browser.key(typed, Browser::Clock::now()));
        CHECK(f.browser.breadcrumb().text() == "j");
        CHECK(f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now()));
        CHECK(cursor_name_is(*f.folder(), "a.txt"));
    }
}

TEST_CASE("keyboard browsing: movement conflicts include selection variants and remain atomic") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    CHECK_FALSE(configure_shortcuts({{"move_up", "k"}}).empty());  // Ctrl+K is the palette.
    CHECK(rummage::views::view_key_event(chord(XKB_KEY_k, {})).keysym == XKB_KEY_k);
    REQUIRE(configure_shortcuts({{"move_up", "k"}, {"command_palette", ""}}).empty());
    CHECK(rummage::views::view_key_event(chord(XKB_KEY_K, {.shift = true})).keysym == XKB_KEY_Up);
    CHECK_FALSE(configure_shortcuts({{"move_up", "Ctrl+k"}}).empty());
    CHECK_FALSE(configure_shortcuts({{"move_down", "j"}, {"new_tab", "Ctrl+j"}}).empty());
    REQUIRE(configure_shortcuts({{"parent_folder", "Ctrl+Alt+p"}, {"open_selected", "Ctrl+o"}})
                .empty());
    CHECK(rummage::views::command_for(chord(XKB_KEY_BackSpace, {})) == Command::none);
    CHECK(rummage::views::command_for(chord(XKB_KEY_Up, {.alt = true})) == Command::none);
    CHECK(rummage::views::command_for(chord(XKB_KEY_Return, {})) == Command::none);
    Fixture f;
    f.reset(Browser::Mode::details);
    CHECK(f.browser.key(chord(XKB_KEY_o, {.ctrl = true}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == f.root / "sub");
    CHECK(f.browser.key(chord(XKB_KEY_p, {.ctrl = true, .alt = true}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == f.root);
}

TEST_CASE("keyboard browsing: explicit pane focus and expansion commands are bindable") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"focus_first_pane", "Alt+F7"},
                                 {"focus_second_pane", "Alt+F8"},
                                 {"expand_folder", "Alt+F9"},
                                 {"collapse_folder", "Alt+F10"},
                                 {"focus_places", "Alt+F11"}})
                .empty());
    Fixture f;
    f.reset(Browser::Mode::details);
    const auto press = [&](xkb_keysym_t sym, Modifiers mods = {}) {
        CHECK(f.browser.key(chord(sym, mods), Browser::Clock::now()));
    };
    press(XKB_KEY_F9, {.alt = true});
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    press(XKB_KEY_F9, {.alt = true});
    CHECK(f.folder()->path() == f.root / "sub");
    CHECK(cursor_name_is(*f.folder(), "inner.txt"));
    press(XKB_KEY_F10, {.alt = true});
    CHECK(f.folder()->path() == f.root);
    press(XKB_KEY_F10, {.alt = true});
    press(XKB_KEY_Down);
    CHECK(cursor_name_is(*f.folder(), "a.txt"));
    press(XKB_KEY_F3);
    REQUIRE(f.browser.split_visible());
    press(XKB_KEY_F7, {.alt = true});
    CHECK(f.browser.active_pane() == 0);
    press(XKB_KEY_F11, {.alt = true});
    CHECK(f.browser.sidebar().focused());
    press(XKB_KEY_F8, {.alt = true});
    CHECK(f.browser.active_pane() == 1);
    CHECK_FALSE(f.browser.sidebar().focused());
}

TEST_CASE("keyboard browsing: palette keeps sidebar targets and view commands survive remapping") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"open_selected", "Ctrl+o"}, {"parent_folder", "Ctrl+Alt+p"}})
                .empty());
    Fixture f;
    f.reset(Browser::Mode::details, "a.txt");
    f.browser.sidebar().set_focused(true);
    f.browser.key(chord(XKB_KEY_End, {}), Browser::Clock::now());
    auto items = f.browser.palette_items();
    CHECK(std::ranges::find(items, "sidebar:tree", &rummage::views::PaletteItem::id) ==
          items.end());
    CHECK(std::ranges::find(items, "sidebar:places", &rummage::views::PaletteItem::id) ==
          items.end());
    auto found = std::ranges::find(items, "place:open_tab", &rummage::views::PaletteItem::id);
    REQUIRE(found != items.end());
    auto open = found->action;
    f.browser.key(chord(XKB_KEY_Home, {}), Browser::Clock::now());
    open();
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == f.root / "sub");
    CHECK_FALSE(f.browser.sidebar().focused());
    items = f.browser.palette_items();
    found = std::ranges::find(items, "view:parent", &rummage::views::PaletteItem::id);
    REQUIRE(found != items.end());
    found->action();
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.folder()->path() == f.root);
    f.folder()->select_name("sub");
    const auto tabs = f.browser.state().tabs.size();
    CHECK(f.browser.key(chord(XKB_KEY_Return, {.ctrl = true}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.state().tabs.size() == tabs + 1);
    CHECK(f.folder()->path() == f.root / "sub");
}

TEST_CASE("keyboard browsing: new defaults yield to prior custom bindings") {
    ResetBindings reset;
    using rummage::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"go_to", "Alt+d"},
                                 {"rename", "Ctrl+Return"},
                                 {"focus_sidebar", "Shift+F6"},
                                 {"copy", "Ctrl+Alt+Return"}})
                .empty());
    CHECK(rummage::views::keys_for(Command::edit_path).empty());
    CHECK(rummage::views::keys_for(Command::open_selected_tab).empty());
    CHECK(rummage::views::keys_for(Command::focus_files).empty());
    CHECK(rummage::views::keys_for(Command::open_other_pane).empty());
}

TEST_CASE("keyboard browsing: Unicode movement bindings keep character input in editors") {
    ResetBindings reset;
    REQUIRE(rummage::views::configure_shortcuts({{"move_down", "eacute"}}).empty());
    Fixture f;
    f.reset(Browser::Mode::details, "a.txt");
    CHECK(f.browser.key(chord(XKB_KEY_Eacute, {.shift = true}), Browser::Clock::now()));
    CHECK(cursor_name_is(*f.folder(), "b.txt"));
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"a.txt", "b.txt"});
    f.folder()->select_name("a.txt");
    CHECK(f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now()));
    REQUIRE(f.browser.name_editor().active());
    auto typed = chord(XKB_KEY_eacute, {});
    typed.text = "é";
    CHECK(f.browser.key(typed, Browser::Clock::now()));
    CHECK(f.browser.key(chord(XKB_KEY_Return, {}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(fs::exists(f.root / "é.txt"));
    CHECK(fs::exists(f.root / "b.txt"));
}

TEST_CASE("keyboard browsing: path editing can leave Trash") {
    Fixture f;
    f.browser.open(rummage::fs::trash_path());
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.key(chord(XKB_KEY_d, {.alt = true}), Browser::Clock::now()));
    REQUIRE(f.browser.breadcrumb().editing());
    CHECK(f.browser.key(chord(XKB_KEY_Return, {}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK_FALSE(f.browser.breadcrumb().editing());
    CHECK(f.browser.key(chord(XKB_KEY_d, {.alt = true}), Browser::Clock::now()));
    f.browser.breadcrumb().paste(f.root.string());
    CHECK(f.browser.key(chord(XKB_KEY_Return, {}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.picker_folder()->path() == f.root);
}

TEST_CASE("keyboard browsing: relative paths use the displayed location in expanded Details") {
    Fixture f;
    f.reset(Browser::Mode::details);
    f.browser.key(chord(XKB_KEY_Right, {}), Browser::Clock::now());
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    f.browser.key(chord(XKB_KEY_Right, {}), Browser::Clock::now());
    REQUIRE(f.folder()->path() == f.root / "sub");
    CHECK(f.browser.key(chord(XKB_KEY_d, {.alt = true}), Browser::Clock::now()));
    CHECK(f.browser.breadcrumb().text() == f.root.string());
    f.browser.breadcrumb().paste("sub");
    CHECK(f.browser.key(chord(XKB_KEY_Return, {}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK_FALSE(f.browser.breadcrumb().editing());
    CHECK(f.browser.details().root_folder()->path() == f.root / "sub");
}

TEST_CASE("insights: shortcut coexists with inversion and yields to custom chords") {
    ResetBindings reset;
    CHECK(rummage::views::command_for(chord(XKB_KEY_I, {.ctrl = true, .alt = true})) ==
          Command::folder_insights);
    CHECK(rummage::views::command_for(chord(XKB_KEY_I, {.ctrl = true, .shift = true})) ==
          Command::none);
    CHECK(rummage::views::keys_for_binding("invert_selection") == "Ctrl+Shift+I");
    REQUIRE(rummage::views::configure_shortcuts({{"copy", "Ctrl+Alt+i"}}).empty());
    CHECK(rummage::views::keys_for(Command::folder_insights).empty());
    CHECK(rummage::views::command_for(chord(XKB_KEY_I, {.ctrl = true, .alt = true})) ==
          Command::copy);
    REQUIRE(rummage::views::configure_shortcuts({{"folder_insights", "Ctrl+Alt+f"}}).empty());
    CHECK(rummage::views::command_for(chord(XKB_KEY_F, {.ctrl = true, .alt = true})) ==
          Command::folder_insights);
}
