#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/shortcuts.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::Folder;
using rummage::views::Shortcut;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace scope = rummage::views::scope;

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

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/{P2.ORF}}, shown in the details.
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
        fs::create_directories(root / "photos");
        fs::create_directories(root / "album");
        for (const char* name : {"photos/P1.ORF", "photos/P1.ORF.xmp", "photos/P2.ORF"}) {
            std::ofstream{root / name} << name;
        }
        std::ofstream{root / "album" / "P2.ORF"} << "album's own";
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> folder() { return browser.details().folder(); }

    // folder shown, these selected.
    void show(const fs::path& folder_path, const Names& selected) {
        browser.open(folder_path);
        browser.set_mode(Browser::Mode::details);  // after open: a mode needs a folder
        REQUIRE(settle());
        if (selected.empty()) {
            return;
        }
        folder()->select_name(selected.front());
        for (std::size_t i = 1; i < selected.size(); ++i) {
            const auto& entries = folder()->entries();
            const auto it = std::ranges::find(entries, selected[i], &rummage::fs::Entry::name);
            REQUIRE(it != entries.end());
            folder()->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                                 rummage::views::SelectMode::toggle);
        }
    }
};

}  // namespace

TEST_CASE(
    "links: external cut clipboard creates links from several folders without moving sources") {
    Fixture f;
    std::ofstream{f.root / "note.txt"} << "note";
    f.browser.set_home_trash(f.dir.path / "Trash");
    f.show(f.root / "album", {});
    const std::vector<fs::path> paths{f.root / "photos" / "P1.ORF", f.root / "note.txt"};
    bool cleared = false;
    rummage::views::SystemClipboard clipboard;
    clipboard.types = [] {
        return std::vector<std::string>{std::string{rummage::views::kGnomeFiles}};
    };
    clipboard.clear = [&] { cleared = true; };
    clipboard.read = [&](const auto&, auto done) {
        f.loop.post([done, paths] { done(rummage::views::gnome_copied_files(true, paths)); });
    };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.browser.paste_links(rummage::fs::LinkKind::relative);
    REQUIRE(f.settle());
    CHECK_FALSE(cleared);
    CHECK(fs::read_symlink(f.root / "album" / "P1.ORF") == "../photos/P1.ORF");
    CHECK(fs::read_symlink(f.root / "album" / "note.txt") == "../note.txt");
    CHECK_FALSE(fs::exists(f.root / "album" / "P1.ORF.xmp"));
    for (const auto& source : paths)
        CHECK(fs::exists(source));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P2.ORF"});
    for (const auto& source : paths)
        CHECK(fs::exists(source));
}

TEST_CASE("paste: Ctrl+C then Ctrl+V copies, sidecars along, and selects what arrived") {
    Fixture f;
    f.show(f.root / "photos", {"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Ctrl+V copies “P1.ORF” where you want them");
    CHECK(f.browser.cut_marks().names.empty());

    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.root / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.browser.status_bar().message().starts_with("Copied 2 files ("));
    CHECK_FALSE(f.browser.status_bar().transfer().has_value());
    REQUIRE(run_until(f.loop, [&] { return f.folder()->cursor_entry() != nullptr; }));
    CHECK(f.folder()->cursor_entry()->name == "P1.ORF");

    // Pasting again: the copy stays in the clipboard; in its own folder it keeps both.
    f.show(f.root / "photos", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "photos") ==
          Names{"P1 2.ORF", "P1 2.ORF.xmp", "P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("paste: a completed folder appears in an empty details view without refreshing") {
    Fixture f;
    fs::create_directory(f.root / "empty");
    f.show(f.root, {"photos"});
    REQUIRE(f.press(key(XKB_KEY_c, {.ctrl = true})));
    f.show(f.root / "empty", {});
    const auto destination = f.folder();
    REQUIRE(destination->entries().empty());

    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(run_until(f.loop,
                      [&] { return !f.browser.loading() && destination->entries().size() == 1; }));
    CHECK(f.folder() == destination);
    CHECK(destination->entries().front().name == "photos");
    CHECK(destination->entries().front().is_directory());
    CHECK(listing(f.root / "empty" / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.root / "empty") == Names{"photos"});
}

TEST_CASE("paste: Ctrl+X fades, Escape keeps them, Ctrl+V moves once") {
    Fixture f;
    f.show(f.root / "photos", {"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.browser.cut_marks().names.contains("P1.ORF"));
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK(f.browser.cut_marks().names.empty());
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Nothing to paste: Ctrl+C or Ctrl+X first");

    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.cut_marks().names.empty());  // on its way: nothing left to move
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.root / "photos") == Names{"P2.ORF"});
    CHECK(f.browser.status_bar().message() == "Moved 2 files");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Nothing to paste: Ctrl+C or Ctrl+X first");
}

TEST_CASE("paste: a taken name asks; skip, replace, both for the rest, stop") {
    Fixture f;
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    const auto paste_into_album = [&](const Names& names) {
        f.show(f.root / "photos", names);
        CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
        f.show(f.root / "album", {});
        CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
        REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
        CHECK(dialog.conflict().name == names.front());  // the first name taken
    };

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_space)));  // the card takes every key; nothing else happens
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(f.press(key(XKB_KEY_s)));
    REQUIRE(f.settle());
    CHECK(read_file(f.root / "album" / "P2.ORF") == "album's own");

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    CHECK(read_file(f.root / "album" / "P2.ORF") == "photos/P2.ORF");

    // Both names taken now: "for the rest" answers the second without asking.
    std::ofstream{f.root / "album" / "P1.ORF"} << "album's P1";
    paste_into_album({"P1.ORF", "P2.ORF"});
    CHECK(f.press(key(XKB_KEY_a)));
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.is_open());
    CHECK(listing(f.root / "album") ==
          Names{"P1 2.ORF", "P1 2.ORF.xmp", "P1.ORF", "P2 2.ORF", "P2.ORF"});

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_Escape)));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Stopped: 0 files copied so far");
}

TEST_CASE("paste: Merge for the rest merges folders; the files inside are still asked") {
    Fixture f;
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    for (const char* folder : {"one", "two"}) {
        fs::create_directories(f.root / "photos" / folder);
        fs::create_directories(f.root / "album" / folder);
        std::ofstream{f.root / "photos" / folder / "x.jpg"} << "new";
        std::ofstream{f.root / "album" / folder / "x.jpg"} << "old";
    }
    f.show(f.root / "photos", {"one", "two"});
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "one");
    CHECK(f.press(key(XKB_KEY_a)));
    CHECK(f.press(key(XKB_KEY_r)));  // merge, for the rest
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "x.jpg");  // one/x.jpg: asked all the same
    CHECK(f.press(key(XKB_KEY_s)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "x.jpg");  // two merged without a question, its x.jpg asked
    CHECK(dialog.conflict().destination == f.root / "album" / "two");
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    CHECK(read_file(f.root / "album" / "one" / "x.jpg") == "old");
    CHECK(read_file(f.root / "album" / "two" / "x.jpg") == "new");
}

TEST_CASE("paste: Shift+Escape stops a transfer on its way, and says when there is none") {
    Fixture f;
    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_Escape, {.shift = true})));
    CHECK(f.browser.status_bar().message() == "Nothing is being copied, moved or deleted");

    // A copy held on a question: the card put away unanswered, the transfer still waits.
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.root / "photos", {"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    dialog.close();
    CHECK(f.browser.loading());
    CHECK(f.press(key(XKB_KEY_Escape, {.shift = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Stopped: 0 files copied so far");
    CHECK(read_file(f.root / "album" / "P2.ORF") == "album's own");
}

TEST_CASE("paste: every key listed for the question works on it") {
    Fixture f;
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    for (const Shortcut& shortcut : rummage::views::shortcuts()) {
        if ((shortcut.scope & scope::kConflict) == 0) {
            continue;
        }
        for (const xkb_keysym_t keysym : shortcut.keysyms) {
            if (keysym == XKB_KEY_NoSymbol) {
                continue;
            }
            f.show(f.root / "photos", {"P2.ORF"});
            CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
            f.show(f.root / "album", {});
            CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
            REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
            CHECK(f.press(key(keysym, shortcut.modifiers)));
            if (dialog.is_open()) {  // "for the rest" only ticks the box
                CHECK(f.press(key(XKB_KEY_s)));
            }
            REQUIRE(f.settle());
        }
    }
}

TEST_CASE("paste: a rebound stop command works from Quick Look and the palette") {
    struct ResetBindings {
        ~ResetBindings() { (void)rummage::views::configure_shortcuts({}); }
    } reset;

    REQUIRE(rummage::views::configure_shortcuts({{"stop_transfer", "Ctrl+Alt+s"}}).empty());
    Fixture f;
    f.show(f.root / "photos", {"P2.ORF"});
    f.browser.toggle_split();
    f.show(f.root / "album", {});
    f.browser.focus_pane(0);
    f.browser.details().folder()->select_name("P2.ORF");
    f.browser.transfer_to_other_pane(false);
    auto& dialog = f.browser.conflict_dialog();
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    dialog.close();  // leave the worker waiting so cancellation cannot race a finished copy
    f.browser.focus_pane(1);

    SUBCASE("Quick Look keeps Escape for closing, and uses the configured stop chord") {
        f.folder()->select_name("P2.ORF");
        REQUIRE(f.press(key(XKB_KEY_space)));
        REQUIRE(f.browser.quick_look().is_open());
        REQUIRE(f.press(key(XKB_KEY_Escape)));
        CHECK_FALSE(f.browser.quick_look().is_open());
        CHECK(f.browser.loading());
        REQUIRE(f.press(key(XKB_KEY_space)));
        REQUIRE(f.browser.quick_look().is_open());
        REQUIRE(f.press(key(XKB_KEY_s, {.ctrl = true, .alt = true})));
        CHECK(f.browser.quick_look().is_open());
    }
    SUBCASE("palette offers the running transfer with its configured chord") {
        const auto items = f.browser.palette_items();
        const auto stop =
            std::ranges::find(items, "command:stop_transfer", &rummage::views::PaletteItem::id);
        REQUIRE(stop != items.end());
        CHECK(stop->keys == "Ctrl+Alt+s");
        REQUIRE(f.press(key(XKB_KEY_k, {.ctrl = true})));
        f.browser.command_palette().set_query("Stop the Transfer");
        REQUIRE(f.browser.command_palette().highlighted());
        REQUIRE(f.press(key(XKB_KEY_Return)));
        CHECK_FALSE(f.browser.command_palette().is_open());
    }
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Stopped: 0 files copied so far");
    CHECK(read_file(f.root / "album" / "P2.ORF") == "album's own");
    const auto items = f.browser.palette_items();
    CHECK(std::ranges::find(items, "command:stop_transfer", &rummage::views::PaletteItem::id) ==
          items.end());
}
