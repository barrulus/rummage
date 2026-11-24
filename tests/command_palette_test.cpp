#include "rummage/views/command_palette.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/name_pattern.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_clipboard.hpp"
#include "rummage/views/fuzzy.hpp"
#include "rummage/views/shortcuts.hpp"
#include "rummage/views/state.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::Command;
using rummage::views::CommandPalette;
using rummage::views::fit_label;
using rummage::views::fuzzy_positions;
using rummage::views::fuzzy_score;
using rummage::views::FuzzyQuery;
using rummage::views::LabelRun;
using rummage::views::PaletteItem;
using rummage::views::State;
using rummage::views::SystemClipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

KeyEvent typed(char c) { return key(static_cast<xkb_keysym_t>(c), {}, std::string(1, c)); }

constexpr Modifiers kCtrl{.ctrl = true};

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

// The best match first: the labels of what the palette shows.
std::vector<std::string> labels(const CommandPalette& palette) {
    std::vector<std::string> out;
    for (const std::size_t index : palette.shown()) {
        out.push_back(palette.items()[index].label);
    }
    return out;
}

std::vector<PaletteItem> plain_items(const std::vector<std::string>& names) {
    std::vector<PaletteItem> items;
    items.reserve(names.size());
    for (const std::string& name : names) {
        items.push_back({.id = name,
                         .label = name,
                         .keys = {},
                         .remember = true,
                         .action = {},
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    }
    return items;
}

}  // namespace

TEST_CASE("fuzzy: in order anywhere, any case, best placement first") {
    CHECK(fuzzy_score("", "anything") == 0.0);
    CHECK(fuzzy_score("ntab", "New Tab"));
    CHECK(fuzzy_score("NEW", "New Tab"));  // case does not matter
    CHECK_FALSE(fuzzy_score("xyz", "New Tab"));
    CHECK_FALSE(fuzzy_score("New Tab and more", "New Tab"));  // longer than the text
    // Word starts and runs win.
    const auto score = [](std::string_view query, std::string_view text) {
        const std::optional<double> found = fuzzy_score(query, text);
        REQUIRE(found);
        return found.value_or(0.0);
    };
    CHECK(score("nt", "New Tab") > score("nt", "Open in Terminal"));
    CHECK(score("tab", "New Tab") > score("tab", "Show the Details Table"));
    CHECK(fuzzy_positions("nt", "New Tab") == std::vector<std::size_t>{0, 4});
    CHECK(fuzzy_positions("xyz", "New Tab").empty());
    CHECK(fuzzy_positions("", "New Tab").empty());
    // UTF-8 bytes are compared as they are.
    CHECK(fuzzy_score("città", "Go to Città"));
    CHECK(fuzzy_positions("tà", "Città").size() == 3);
}

TEST_CASE("palette: filters as it is typed, the best first, recent first when empty") {
    CommandPalette palette{[] {}};
    palette.open(plain_items({"Open in Terminal", "New Folder", "New Tab", "Next Tab",
                              "Show Hidden Files", "Sort by Name"}),
                 {"Sort by Name", "gone", "New Tab"}, "Type a command");
    REQUIRE(palette.is_open());
    // Nothing typed: the recently used (those still offered), then the rest in order.
    CHECK(labels(palette) == std::vector<std::string>{"Sort by Name", "New Tab", "Open in Terminal",
                                                      "New Folder", "Next Tab",
                                                      "Show Hidden Files"});
    CHECK(palette.highlighted() == 0);

    palette.key(typed('n'));
    palette.key(typed('f'));
    CHECK(palette.query() == "nf");
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(labels(palette).front() == "New Folder");

    palette.set_query("ntab");
    const std::vector<std::string> tabs = labels(palette);
    CHECK(tabs.front() == "New Tab");
    CHECK(std::ranges::find(tabs, "Next Tab") != tabs.end());

    palette.set_query("zzz");
    CHECK(palette.shown().empty());
    CHECK_FALSE(palette.highlighted());
    palette.key(key(XKB_KEY_Return));  // nothing to run
    CHECK(palette.is_open());

    palette.key(key(XKB_KEY_BackSpace));
    palette.key(key(XKB_KEY_BackSpace));
    palette.key(key(XKB_KEY_BackSpace));
    CHECK(palette.query().empty());
    CHECK(palette.shown().size() == 6);
}

TEST_CASE("palette: a label too long keeps its match in sight, lit (issue #25)") {
    // One unit a character ("…" too), whatever its bytes.
    const auto width = [](std::string_view text) {
        return static_cast<float>(std::ranges::count_if(
            text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
    };
    const auto text_of = [](const std::vector<LabelRun>& runs) {
        std::string text;
        for (const LabelRun& run : runs) {
            text += run.text;
        }
        return text;
    };
    const auto lit_of = [](const std::vector<LabelRun>& runs) {
        std::vector<std::string> lit;
        for (const LabelRun& run : runs) {
            if (run.lit) {
                lit.push_back(run.text);
            }
        }
        return lit;
    };

    // It fits: the whole label, its matches lit.
    const std::vector<std::size_t> nt{0, 4};
    CHECK(fit_label("New Tab", nt, false, 100.0F, width) ==
          std::vector<LabelRun>{{"N", true}, {"ew ", false}, {"T", true}, {"ab", false}});

    // A path: its name whole, as much of its start as fits.
    const std::string path = "Documents/Projects/2026/report.pdf";
    const std::vector<std::size_t> rep = FuzzyQuery{"rep"}.path_positions(path);
    const std::vector<LabelRun> cut = fit_label(path, rep, true, 20.0F, width);
    CHECK(text_of(cut) == "Document…/report.pdf");
    CHECK(lit_of(cut) == std::vector<std::string>{"rep"});

    // A name alone too long (stradus64's): from its match on, the match lit.
    const std::string name =
        "2026-11-16 @absolute_bajoding human slop existed long  before LLMs [UOqxEJwvhpY].webm";
    const std::vector<std::size_t> slo = FuzzyQuery{"slo"}.path_positions(name);
    REQUIRE_FALSE(slo.empty());
    const std::vector<LabelRun> shown = fit_label(name, slo, true, 30.0F, width);
    CHECK(width(text_of(shown)) <= 30.0F);
    CHECK(text_of(shown).starts_with("…"));
    CHECK(lit_of(shown) == std::vector<std::string>{"slo"});

    // Cuts fall between characters, never inside one.
    const std::vector<std::size_t> fifth{8};  // the fifth "à"
    const std::vector<LabelRun> accents = fit_label("àààààààààà", fifth, false, 5.0F, width);
    CHECK(text_of(accents) == "…ààà…");
    CHECK(lit_of(accents) == std::vector<std::string>{"à"});
    // Not one character fits: only "…".
    CHECK(text_of(fit_label("New Tab", nt, false, 1.5F, width)) == "…");
}

TEST_CASE("palette: commands take terms in any order; Space is always a space there") {
    CommandPalette palette{[] {}};
    palette.open(plain_items({"Open in Terminal", "Copy Path", "Paste", "New Tab"}), {},
                 "Type a command");
    for (const char c : std::string_view{"pa co"}) {
        palette.key(typed(c));
    }
    CHECK(palette.query() == "pa co");
    CHECK(labels(palette) == std::vector<std::string>{"Copy Path"});
    // No Quick Look among commands: from the list too, Space types.
    palette.key(key(XKB_KEY_Down));
    palette.key(typed(' '));
    CHECK(palette.is_open());
    CHECK(palette.query() == "pa co ");
}

TEST_CASE("palette: arrows move within the list, Enter runs and closes, Escape closes") {
    CommandPalette palette{[] {}};
    std::vector<std::string> ran;
    std::vector<std::string> chosen;
    palette.set_chosen_handler([&](const std::string& id) { chosen.push_back(id); });
    std::vector<PaletteItem> items = plain_items({"One", "Two", "Three"});
    for (PaletteItem& item : items) {
        item.action = [&ran, label = item.label] { ran.push_back(label); };
    }
    items[2].remember = false;
    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_Up));  // already at the top
    CHECK(palette.highlighted() == 0);
    palette.key(key(XKB_KEY_Down));
    palette.key(key(XKB_KEY_Down));
    palette.key(key(XKB_KEY_Down));  // stays on the last
    CHECK(palette.highlighted() == 2);
    palette.key(key(XKB_KEY_Page_Up));
    CHECK(palette.highlighted() == 0);
    palette.key(key(XKB_KEY_Page_Down));
    CHECK(palette.highlighted() == 2);
    palette.key(key(XKB_KEY_Up));
    palette.key(key(XKB_KEY_Return));
    CHECK_FALSE(palette.is_open());
    CHECK(ran == std::vector<std::string>{"Two"});
    CHECK(chosen == std::vector<std::string>{"Two"});

    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_End));  // the field's: the caret
    palette.key(key(XKB_KEY_Page_Down));
    palette.choose_highlighted();
    CHECK(ran == std::vector<std::string>{"Two", "Three"});
    CHECK(chosen == std::vector<std::string>{"Two"});  // Three is not remembered

    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_Escape));
    CHECK_FALSE(palette.is_open());
    CHECK(ran.size() == 2);

    // A paste: its first line, without control characters.
    palette.open(items, {}, "");
    palette.paste("Th\tree\nsecond line");
    CHECK(palette.query() == "Three");
    CHECK(labels(palette).front() == "Three");
}

TEST_CASE("palette: the selection cut out of the query, the list filters again") {
    CommandPalette palette{[] {}};
    palette.open(plain_items({"One", "Two", "Three", "Four", "Five", "Six"}), {}, "");
    palette.set_query("zzz");
    CHECK(palette.shown().empty());
    CHECK(palette.cut().empty());  // nothing selected
    CHECK(palette.query() == "zzz");
    palette.key(key(XKB_KEY_a, kCtrl));
    CHECK(palette.field().selected_text() == "zzz");
    CHECK(palette.cut() == "zzz");
    CHECK(palette.query().empty());
    CHECK(palette.shown().size() == 6);
}

TEST_CASE("palette: filtering 500 items takes well under a millisecond a key") {
    std::vector<std::string> names;
    names.reserve(500);
    for (int i = 0; i < 500; ++i) {
        names.push_back(std::format("Go to Some Folder Number {} in the Photos", i));
    }
    CommandPalette palette{[] {}};
    palette.open(plain_items(names), {}, "");
    const std::string query = "photos";
    const auto start = std::chrono::steady_clock::now();
    for (const char c : query) {
        palette.key(typed(c));
    }
    const auto each = (std::chrono::steady_clock::now() - start) / query.size();
    const double ms = std::chrono::duration<double, std::milli>(each).count();
    MESSAGE(std::format("filtering 500 items: {:.3f} ms a key", ms));
    CHECK(palette.shown().size() == 500);
    CHECK(ms < 10.0);  // the target is 1 ms in a release build; sanitizers are slower
}

namespace {

// a/{one.txt, sub/}, b/, in the columns at a, with two places.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    fs::path b = dir.path / "b";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(a / "sub");
        fs::create_directories(b);
        std::ofstream{a / "one.txt"} << "1";
        browser.set_home_trash(dir.path / "Trash");
        browser.set_places({{.section = rummage::fs::Place::Section::places,
                             .label = "Bee",
                             .path = b,
                             .icon = "folder",
                             .disk = {}}});
        browser.open(a);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    [[nodiscard]] std::set<std::string> ids() {
        std::set<std::string> out;
        for (const PaletteItem& item : browser.palette_items()) {
            out.insert(item.id);
        }
        return out;
    }

    // Opens the palette, types query, runs the best match.
    void run(std::string_view query) {
        REQUIRE(press(key(XKB_KEY_k, kCtrl)));
        REQUIRE(browser.command_palette().is_open());
        browser.command_palette().set_query(std::string{query});
        REQUIRE(browser.command_palette().highlighted());
        press(key(XKB_KEY_Return));
        CHECK_FALSE(browser.command_palette().is_open());
        REQUIRE(settle());
    }
};

}  // namespace

TEST_CASE("palette in the browser: Ctrl+K opens and closes it, and it takes every key") {
    Fixture f;
    CHECK(f.press(key(XKB_KEY_k, kCtrl)));
    REQUIRE(f.browser.command_palette().is_open());
    CHECK(f.press(key(XKB_KEY_2, kCtrl)));  // not the grid: the palette's
    CHECK(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.press(typed('g')));
    CHECK(f.browser.command_palette().query() == "g");
    CHECK(f.press(key(XKB_KEY_K, kCtrl)));  // Ctrl+K again (Caps Lock on)
    CHECK_FALSE(f.browser.command_palette().is_open());
    // The context menu gives way to it.
    f.browser.open_menu(100, 100, false);
    REQUIRE(f.browser.context_menu().is_open());
    f.browser.open_palette();
    CHECK_FALSE(f.browser.context_menu().is_open());
    CHECK(f.browser.command_palette().is_open());
}

TEST_CASE("palette in the browser: Ctrl+C and Ctrl+X take the query's selection as text") {
    Fixture f;
    std::vector<SystemClipboard::Format> offered;
    SystemClipboard clipboard;
    clipboard.offer = [&](std::vector<SystemClipboard::Format> formats) {
        offered = std::move(formats);
    };
    f.browser.set_system_clipboard(std::move(clipboard));
    CHECK(f.press(key(XKB_KEY_k, kCtrl)));
    for (const char c : std::string_view{"grid"}) {
        CHECK(f.press(typed(c)));
    }
    CHECK(f.press(key(XKB_KEY_a, kCtrl)));
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front() == SystemClipboard::Format{"text/plain;charset=utf-8", "grid"});
    CHECK(f.browser.command_palette().query() == "grid");
    offered.clear();
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().second == "grid");
    CHECK(f.browser.command_palette().query().empty());
    CHECK(f.browser.command_palette().is_open());
}

TEST_CASE("palette in the browser: every command is offered where it can be used") {
    Fixture f;
    std::set<std::string> seen = f.ids();
    // A file under the cursor: what acts on it.
    f.press(key(XKB_KEY_Down));  // sub → one.txt (columns: a's entries)
    seen.merge(f.ids());
    // Two tabs, the grid, history behind.
    f.browser.new_tab();
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    CHECK_FALSE(f.ids().contains("command:reopen_tab"));  // nothing closed yet
    seen.merge(f.ids());                                  // back
    f.browser.back();
    REQUIRE(f.settle());
    seen.merge(f.ids());
    // Something to paste, undo and redo.
    f.browser.open(f.a);
    REQUIRE(f.settle());
    f.press(key(XKB_KEY_Down));
    f.press(key(XKB_KEY_c, kCtrl));
    seen.merge(f.ids());

    f.browser.close_tab(0);
    seen.merge(f.ids());

    // Pane commands require two different folders and a source selection.
    f.browser.set_mode(Browser::Mode::details);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    f.browser.details().folder()->select_name("sub");
    seen.merge(f.ids());
    f.browser.details().folder()->select_name("one.txt");
    seen.merge(f.ids());

    // Every command of the shortcuts but those the palette has no use for.
    const std::set<Command> not_in_palette{
        Command::none, Command::context_menu, Command::select_tab, Command::command_palette,
        Command::undo, Command::redo, Command::stop_transfer,
        // Keys while looking at the info pane: the palette closes on a pick.
        Command::scroll_preview_up, Command::scroll_preview_down, Command::scroll_preview_page_up,
        Command::scroll_preview_page_down};
    const std::vector<std::pair<Command, const char*>> commands{
        {Command::edit_path, "edit_path"},
        {Command::focus_files, "focus_files"},
        {Command::focus_places, "focus_places"},
        {Command::focus_first_pane, "focus_first_pane"},
        {Command::focus_second_pane, "focus_second_pane"},
        {Command::open_selected, "open"},
        {Command::open_selected_tab, "open_tab"},
        {Command::expand_folder, "expand_folder"},
        {Command::collapse_folder, "collapse_folder"},
        {Command::columns_view, "columns_view"},
        {Command::grid_view, "grid_view"},
        {Command::details_view, "details_view"},
        {Command::toggle_info, "toggle_info"},
        {Command::toggle_sidebar, "toggle_sidebar"},
        {Command::focus_sidebar, "focus_sidebar"},
        {Command::quick_look, "quick_look"},
        {Command::show_shortcuts, "show_shortcuts"},
        {Command::show_settings, "show_settings"},
        {Command::properties, "properties"},
        {Command::folder_insights, "folder_insights"},
        {Command::rename, "rename"},
        {Command::new_folder, "new_folder"},
        {Command::copy, "copy"},
        {Command::copy_path, "copy_path"},
        {Command::cut, "cut"},
        {Command::paste, "paste"},
        {Command::trash, "trash"},
        {Command::erase, "erase"},
        {Command::back, "back"},
        {Command::forward, "forward"},
        {Command::toggle_hidden, "toggle_hidden"},
        {Command::toggle_thumbnails, "toggle_thumbnails"},
        {Command::toggle_extensions, "toggle_extensions"},
        {Command::add_bookmark, "add_bookmark"},
        {Command::new_tab, "new_tab"},
        {Command::close_tab, "close_tab"},
        {Command::reopen_tab, "reopen_tab"},
        {Command::next_tab, "next_tab"},
        {Command::previous_tab, "previous_tab"},
        {Command::find_files, "find_files"},
        {Command::select_pattern, "select_pattern"},
        {Command::reset_folder_view, "reset_folder_view"},
        {Command::filter_names, "filter_names"},
        {Command::toggle_split, "toggle_split"},
        {Command::switch_pane, "switch_pane"},
        {Command::swap_panes, "swap_panes"},
        {Command::orient_panes, "orient_panes"},
        {Command::open_other_pane, "open_other_pane"},
        {Command::same_other_pane, "same_other_pane"},
        {Command::copy_other_pane, "copy_other_pane"},
        {Command::move_other_pane, "move_other_pane"},
        {Command::go_to, "go_to"},
    };
    for (const rummage::views::Shortcut& shortcut : rummage::views::shortcuts()) {
        if (shortcut.command == Command::parent_folder) {
            CHECK(seen.contains("view:parent"));
            continue;
        }
        if (not_in_palette.contains(shortcut.command)) {
            continue;
        }
        const auto it =
            std::ranges::find(commands, shortcut.command, &std::pair<Command, const char*>::first);
        if (it == commands.end()) {
            // open_terminal: only with a terminal handler (none in the tests).
            CHECK(shortcut.command == Command::open_terminal);
            continue;
        }
        const std::string id = std::string{"command:"} + it->second;
        CAPTURE(id);
        CHECK(seen.contains(id));
    }
    for (const char* id : {"sort:size", "sort:descending", "view:zoom_in", "view:select_all",
                           "view:invert_selection", "view:parent", "go:", "command:empty_trash",
                           "command:new_file", "command:open"}) {
        if (std::string_view{id} == "go:") {
            CHECK(seen.contains("go:" + f.b.string()));
        } else {
            CAPTURE(id);
            CHECK(seen.contains(id));
        }
    }
    // The other tab, by number; no Quit without a handler.
    CHECK(seen.contains("tab:0"));  // from the second tab, the first
    CHECK_FALSE(seen.contains("command:quit"));
    f.browser.set_quit_handler([] {});
    CHECK(f.ids().contains("command:quit"));
}

TEST_CASE("palette in the browser: an item does what its key does") {
    Fixture f;
    f.run("show grid");
    CHECK(f.browser.mode() == Browser::Mode::grid);
    f.run("hidden");
    CHECK(f.browser.shows_hidden());
    f.run("hidden");  // now "Hide Hidden Files"
    CHECK_FALSE(f.browser.shows_hidden());
    f.run("new tab");
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 1);
    f.run("tab 1");
    CHECK(f.browser.active_tab() == 0);
    f.run("sort size");
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    f.run("go to bee");
    REQUIRE(f.browser.columns().column_count() > 0);
    CHECK(f.browser.state().palette_recent.front() == "go:" + f.b.string());

    // The keys' path: a view switch from the palette and from Ctrl+3 leave the same state.
    Fixture g;
    f.run("show details");
    g.press(key(XKB_KEY_3, kCtrl));
    CHECK(f.browser.mode() == g.browser.mode());
}

TEST_CASE("palette in the browser: Invert Selection does what Ctrl+Shift+I does") {
    Fixture f;
    Fixture g;
    f.run("invert");
    REQUIRE(g.press(key(XKB_KEY_I, {.ctrl = true, .shift = true})));
    const std::vector<std::string> inverted =
        f.browser.columns().focused_folder()->selected_names();
    CHECK_FALSE(inverted.empty());
    CHECK(inverted == g.browser.columns().focused_folder()->selected_names());
    CHECK(f.browser.state().palette_recent.front() == "view:invert_selection");
}

TEST_CASE("palette in the browser: the recently used come back first, and in state.toml") {
    Fixture f;
    f.run("show grid");
    f.run("new tab");
    f.run("hidden");
    const std::vector<std::string> recent = f.browser.palette_recent();
    REQUIRE(recent.size() == 3);
    CHECK(recent[0] == "command:toggle_hidden");
    CHECK(recent[1] == "command:new_tab");
    CHECK(recent[2] == "command:grid_view");

    f.browser.open_palette();
    // "Show Grid" is not offered in the grid; the other two come first, the latest first (the
    // hidden files' item under its other label).
    const auto& palette = f.browser.command_palette();
    REQUIRE(palette.shown().size() > 2);
    CHECK(palette.items()[palette.shown()[0]].id == "command:toggle_hidden");
    CHECK(palette.items()[palette.shown()[0]].label == "Hide Hidden Files");
    CHECK(palette.items()[palette.shown()[1]].id == "command:new_tab");
    f.browser.command_palette().close();

    // Through state.toml and back.
    const State state = f.browser.state();
    const rummage::views::StateResult read =
        rummage::views::parse_state(rummage::views::format_state(state), "state.toml");
    CHECK(read.problems.empty());
    CHECK(read.state.palette_recent == recent);
    Fixture g;
    g.browser.apply(read.state);
    CHECK(g.browser.palette_recent() == recent);
}

TEST_CASE("state: palette.recent keeps strings only, ten at most") {
    std::string list;
    for (int i = 0; i < 12; ++i) {
        list += std::format("\"id{}\", ", i);
    }
    const rummage::views::StateResult read =
        rummage::views::parse_state("[palette]\nrecent = [" + list + "3, \"\"]\n", "s.toml");
    CHECK(read.state.palette_recent.size() == State::kMaxRecent);
    CHECK(read.state.palette_recent.front() == "id0");
    const rummage::views::StateResult wrong =
        rummage::views::parse_state("[palette]\nrecent = \"id\"\n", "s.toml");
    CHECK(wrong.state.palette_recent.empty());
    CHECK(wrong.problems.size() == 1);
}

namespace {

struct PatternFixture : Fixture {
    PatternFixture() {
        for (const char* name : {"P1.ORF", "P1.JPG", "p2.orf", "IMG 001.jpg", ".hidden.orf"}) {
            std::ofstream{a / name} << name;
        }
        REQUIRE(run_until(loop, [this] { return folder()->entries().size() == 6; }));
    }

    std::shared_ptr<rummage::views::Folder> folder() { return browser.columns().focused_folder(); }

    void type(std::string_view text) {
        for (const char c : text) {
            press(c == ' ' ? key(XKB_KEY_space, {}, " ") : typed(c));
        }
    }
};

}  // namespace

TEST_CASE("select by name: matching ignores case and leaves hidden files out while hidden") {
    PatternFixture f;
    auto names = [&f](const std::vector<std::size_t>& rows) {
        std::set<std::string> out;
        for (const std::size_t r : rows) {
            out.insert(f.folder()->entries()[r].name);
        }
        return out;
    };
    CHECK(names(f.folder()->matching("*.orf")) == std::set<std::string>{"P1.ORF", "p2.orf"});
    CHECK(f.folder()->matching("").empty());
    CHECK(f.folder()->matching("*.xyz").empty());
    f.press(key(XKB_KEY_h, kCtrl));
    REQUIRE(f.settle());
    CHECK(names(f.folder()->matching("*.orf")) ==
          std::set<std::string>{"P1.ORF", "p2.orf", ".hidden.orf"});
}

TEST_CASE(
    "select by name: the rows become the selection or join it; the cursor moves only off them") {
    PatternFixture f;
    auto folder = f.folder();
    folder->select_name("one.txt");
    folder->select_rows(folder->matching("*.orf"), false);
    CHECK(folder->selected_names().size() == 2);
    CHECK_FALSE(folder->is_selected("one.txt"));
    REQUIRE(folder->cursor_entry() != nullptr);
    CHECK(rummage::fs::matches_name(folder->cursor_entry()->name, "*.orf"));
    folder->select_name("p2.orf");
    folder->select_rows(folder->matching("*.orf"), false);
    CHECK(folder->cursor_entry()->name == "p2.orf");
    folder->select_name("sub");
    folder->select_rows(folder->matching("*.orf"), true);
    CHECK(folder->selected_count() == 3);
    CHECK(folder->is_selected("sub"));
    folder->select_rows({}, false);
    CHECK(folder->selected_count() == 3);
}

TEST_CASE(
    "palette in the browser: Ctrl+S selects by name, counts as it is typed, Escape leaves the "
    "selection") {
    PatternFixture f;
    REQUIRE(f.press(key(XKB_KEY_s, kCtrl)));
    auto& palette = f.browser.command_palette();
    REQUIRE(palette.is_open());
    CHECK(palette.external());
    f.type("*.orf");
    CHECK(palette.status() == "2 of 6 match");
    REQUIRE(palette.items().size() == 1);
    CHECK(palette.items().front().id == "pattern:select");
    f.press(key(XKB_KEY_Return));
    CHECK_FALSE(palette.is_open());
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"P1.ORF", "p2.orf"});
    f.press(key(XKB_KEY_s, kCtrl));
    f.type("jpg");
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(palette.is_open());
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"P1.ORF", "p2.orf"});
    f.press(key(XKB_KEY_s, kCtrl));
    f.press(key(XKB_KEY_S, kCtrl));
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("palette in the browser: Ctrl+K, Ctrl+F and Ctrl+S switch between their cards") {
    PatternFixture f;
    auto& palette = f.browser.command_palette();
    f.press(key(XKB_KEY_s, kCtrl));
    REQUIRE(palette.external());
    f.press(key(XKB_KEY_f, kCtrl));
    CHECK(palette.is_open());
    CHECK(f.browser.file_finder() != nullptr);
    f.press(key(XKB_KEY_s, kCtrl));
    CHECK(palette.is_open());
    CHECK(f.browser.file_finder() == nullptr);
    f.press(key(XKB_KEY_k, kCtrl));
    CHECK(palette.is_open());
    CHECK_FALSE(palette.external());
    f.press(key(XKB_KEY_s, kCtrl));
    CHECK(palette.external());
    f.press(key(XKB_KEY_k, kCtrl));
    f.press(key(XKB_KEY_k, kCtrl));
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("palette in the browser: a space is typed in a pattern") {
    PatternFixture f;
    f.press(key(XKB_KEY_s, kCtrl));
    f.type("IMG 0*");
    CHECK(f.browser.command_palette().query() == "IMG 0*");
    CHECK(f.browser.command_palette().status() == "1 of 6 match");
    f.type("z");
    CHECK(f.browser.command_palette().status() == "None of 6 match");
    CHECK(f.browser.command_palette().items().empty());
}

TEST_CASE("palette in the browser: a space is typed too with the keyboard on a pattern's row") {
    PatternFixture f;
    auto& palette = f.browser.command_palette();
    f.press(key(XKB_KEY_s, kCtrl));
    f.type("IMG");
    f.press(key(XKB_KEY_Down));
    REQUIRE(palette.in_list());
    REQUIRE(palette.highlighted().has_value());
    f.press(key(XKB_KEY_space, {}, " "));
    CHECK(palette.query() == "IMG ");
    CHECK_FALSE(palette.in_list());
    f.type("0*");
    CHECK(palette.status() == "1 of 6 match");
}

TEST_CASE("palette in the browser: Add to the Selection only when something else is selected") {
    PatternFixture f;
    auto& palette = f.browser.command_palette();
    f.folder()->select_name("P1.ORF");
    f.press(key(XKB_KEY_s, kCtrl));
    f.type("*.orf");
    CHECK(palette.items().size() == 1);
    f.press(key(XKB_KEY_Escape));
    f.folder()->select_name("sub");
    f.press(key(XKB_KEY_s, kCtrl));
    f.type("*.orf");
    REQUIRE(palette.items().size() == 2);
    f.press(key(XKB_KEY_Down));
    f.press(key(XKB_KEY_Return));
    CHECK(f.folder()->selected_count() == 3);
}

TEST_CASE(
    "select by name: literal backslashes agree with filtering and active filters bound selection") {
    CHECK(rummage::fs::matches_name("foo\\bar", "foo\\bar"));
    CHECK_FALSE(rummage::fs::matches_name("foobar", "foo\\bar"));
    PatternFixture f;
    f.browser.set_name_filter("P1");
    f.browser.open_pattern();
    f.type("*.orf");
    CHECK(f.browser.command_palette().status() == "1 of 2 match");
    f.press(key(XKB_KEY_Return));
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"P1.ORF"});
    CHECK(f.browser.name_filter() == "P1");
}

TEST_CASE("select by name: stale matches are checked again before selection") {
    PatternFixture f;
    f.browser.open_pattern();
    f.type("*.orf");
    fs::remove(f.a / "P1.ORF");
    REQUIRE(run_until(f.loop, [&] { return f.folder()->entries().size() == 5; }));
    f.press(key(XKB_KEY_Return));
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"p2.orf"});
}

TEST_CASE("name patterns: explicit modes, case, Unicode and invalid input") {
    using rummage::fs::matches_name;
    using rummage::fs::NamePattern;
    CHECK(matches_name("Photo[1].JPG", "text:[1]"));
    CHECK(matches_name("Photo.JPG", "glob:*.jpg"));
    CHECK_FALSE(matches_name("Photo.JPG", "case:glob:*.jpg"));
    CHECK(matches_name("Photo.JPG", "case:text:Photo"));
    CHECK_FALSE(matches_name("Photo.JPG", "case:text:photo"));
    CHECK(matches_name("IMG_123.JPG", "re:^IMG_[0-9]+\\.jpg$"));
    CHECK_FALSE(matches_name("IMG_abc.JPG", "re:^IMG_[0-9]+\\.jpg$"));
    CHECK(matches_name("école.txt", "re:^ÉCOLE"));
    CHECK(matches_name("re:literal", "text:re:literal"));
    CHECK(matches_name("case:literal", "text:case:literal"));
    for (const auto* invalid : {"re:[", "re:(a)\\1", "re:(?=x)", "glob:[abc", "glob:abc\\"}) {
        CAPTURE(invalid);
        CHECK_FALSE(NamePattern{invalid}.error().empty());
    }
    CHECK_FALSE(NamePattern{std::string(1025, 'a')}.error().empty());
    CHECK(matches_name("]", "glob:[]]"));
    CHECK(matches_name("a", "glob:[[:alpha:]]"));
    CHECK_FALSE(matches_name(std::string(255, 'a') + "!", "re:^(a+)+$"));
    std::string glob = "glob:";
    for (int i = 0; i < 60; ++i)
        glob += "*[a]";
    CHECK_FALSE(matches_name(std::string(255, 'a'), glob + "b"));
    CHECK_FALSE(NamePattern{"glob:[z-a]"}.error().empty());
    CHECK(matches_name("line\nbreak.txt", "glob:*.txt"));
    const auto spec = NamePattern::parse("case:re:^name$");
    CHECK(NamePattern::encode(spec) == "case:re:^name$");
}

TEST_CASE("filter: invalid queries retain the usable view and persist the last valid query") {
    PatternFixture f;
    f.browser.open_tab(f.a, false);
    REQUIRE(f.settle());
    for (const auto mode : {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        f.browser.set_mode(mode);
        f.browser.set_name_filter("re:^P1");
        REQUIRE(f.settle());
        REQUIRE(f.browser.picker_folder()->entries().size() == 2);
        const auto before = f.browser.state();
        f.browser.set_name_filter("re:[");
        CHECK(f.browser.name_filter() == "re:[");
        CHECK(f.browser.picker_folder()->entries().size() == 2);
        CHECK(f.browser.state().tabs.front().filter == "re:^P1");
        CHECK(f.browser.status_bar().message().starts_with("Invalid pattern:"));
        CHECK(f.browser.state().tabs.front().filter == before.tabs.front().filter);
        f.browser.set_name_filter("case:glob:*.ORF");
        f.browser.open_pattern();
        f.type("re:^P[0-9]");
        CHECK(f.browser.command_palette().status() == "1 of 1 match");
        f.press(key(XKB_KEY_Return));
    }
}

TEST_CASE("select by name: invalid regex leaves selection unchanged") {
    PatternFixture f;
    f.folder()->select_name("P1.ORF");
    f.browser.open_pattern();
    f.type("re:[");
    CHECK(f.browser.command_palette().items().empty());
    CHECK(f.browser.command_palette().status().starts_with("Invalid pattern:"));
    f.press(key(XKB_KEY_Return));
    CHECK(f.folder()->selected_names() == std::vector<std::string>{"P1.ORF"});
}

TEST_CASE("filter: keyboard menu changes matching mode and case sensitivity") {
    PatternFixture f;
    f.browser.set_name_filter("P1");
    f.press(key(XKB_KEY_F, {.ctrl = true, .shift = true}));
    REQUIRE(f.browser.filtering());
    f.press(key(XKB_KEY_space, kCtrl));
    REQUIRE(f.browser.context_menu().is_open());
    f.press(key(XKB_KEY_Down));  // Plain text
    f.press(key(XKB_KEY_Return));
    CHECK(f.browser.name_filter() == "text:P1");
    REQUIRE(f.browser.filtering());
    f.press(key(XKB_KEY_space, kCtrl));
    f.press(key(XKB_KEY_End));  // Case sensitive
    f.press(key(XKB_KEY_Return));
    CHECK(f.browser.name_filter() == "case:text:P1");
}
