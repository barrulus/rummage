#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::DirectoryWatcher;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::Column;
using rummage::views::ColumnView;
using rummage::views::Folder;
using rummage::views::GridView;
using rummage::views::SelectMode;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Runs the loop until done() holds or 5 s pass.
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

void write(const fs::path& file, std::string_view text) { std::ofstream{file} << text; }

std::string cursor_name(const Folder& folder) {
    const rummage::fs::Entry* entry = folder.cursor_entry();
    return entry == nullptr ? std::string{} : entry->name;
}

std::string cursor_name(const Column& column) { return cursor_name(column.folder()); }

// Include ambiguous Markdown names; the open test requests content sniffing as drawing would.
// root/{alpha/{beta/{photo.jpg}, notes.txt}, gamma/, readme.md}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    ColumnView& view = browser.columns();
    GridView& grid = browser.grid();

    Fixture() {
        fs::create_directories(root / "alpha" / "beta");
        fs::create_directories(root / "gamma");
        write(root / "alpha" / "beta" / "photo.jpg", "x");
        write(root / "alpha" / "notes.txt", "notes");
        write(root / "readme.md", "# hi");
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    void press(xkb_keysym_t key, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        browser.key(event, Browser::Clock::now());
    }
};

}  // namespace

TEST_CASE("column view: arrows move the cursor and open folders in the next column") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    CHECK(f.view.column_count() == 1);

    f.press(XKB_KEY_Down);  // folders first: alpha
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.column(1).path() == f.root / "alpha");
    REQUIRE(f.settle());

    f.press(XKB_KEY_Right);  // into alpha: the cursor goes to its first entry, beta
    CHECK(f.view.focus() == 1);
    CHECK(cursor_name(f.view.column(1)) == "beta");
    REQUIRE(f.view.column_count() == 3);
    REQUIRE(f.settle());

    f.press(XKB_KEY_Down);  // a file: no column after it
    CHECK(cursor_name(f.view.column(1)) == "notes.txt");
    CHECK(f.view.column_count() == 2);

    f.press(XKB_KEY_Left);  // back: alpha stays open, without a cursor
    CHECK(f.view.focus() == 0);
    CHECK(f.view.column_count() == 2);
    CHECK_FALSE(f.view.column(1).folder().cursor());

    f.press(XKB_KEY_End);  // readme.md
    CHECK(cursor_name(f.view.column(0)) == "readme.md");
    CHECK(f.view.column_count() == 1);
    f.press(XKB_KEY_Up);  // gamma
    CHECK(f.view.column_count() == 2);
    CHECK(f.view.column(1).path() == f.root / "gamma");
}

TEST_CASE("column view: left from the first column opens the parent, cursor on where we were") {
    Fixture f;
    f.view.open(f.root / "alpha");
    REQUIRE(f.settle());
    f.press(XKB_KEY_Left);
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.focus() == 0);
    CHECK(f.view.column(0).path() == f.root);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(f.view.column(1).path() == f.root / "alpha");
}

TEST_CASE("column view: reveal unfolds the columns down to a file") {
    Fixture f;
    f.view.reveal(f.root, f.root / "alpha" / "beta" / "photo.jpg");
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 3);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(cursor_name(f.view.column(1)) == "beta");
    CHECK(cursor_name(f.view.column(2)) == "photo.jpg");
    CHECK(f.view.focus() == 2);
}

TEST_CASE("column view: filtering affects only the last column and preserves ancestors") {
    Fixture f;
    f.view.reveal(f.root, f.root / "alpha" / "beta" / "photo.jpg");
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 3);
    f.browser.set_name_filter("readme");
    REQUIRE(f.view.column_count() == 3);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(cursor_name(f.view.column(1)) == "beta");
    CHECK(f.view.column(0).folder().entries().size() == 3);
    CHECK(f.view.column(1).folder().entries().size() == 2);
    CHECK(f.view.column(2).folder().entries().empty());
    f.browser.set_name_filter("photo");
    REQUIRE(f.view.column(2).folder().entries().size() == 1);
    CHECK(f.view.column(2).folder().entries().front().name == "photo.jpg");
    // Close the child column: its parent now receives the filter.
    f.view.column(1).folder().set_cursor(std::nullopt);
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.column(1).folder().entries().empty());
    CHECK(f.view.column(0).folder().entries().size() == 3);
    f.browser.set_name_filter("");
    REQUIRE(f.view.column(1).folder().entries().size() == 2);
    f.view.column(1).folder().select_name("beta");
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 3);
    f.browser.set_entry_filter(
        [](const rummage::fs::Entry& entry) { return entry.name != "beta"; });
    CHECK(f.view.column_count() == 3);
    CHECK(cursor_name(f.view.column(1)) == "beta");
}

TEST_CASE("column view: a folder deleted or renamed under the cursor updates the columns") {
    Fixture f;
    f.view.reveal(f.root, f.root / "alpha" / "beta");
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 3);

    // Renamed: the cursor follows it, the next column shows it under its new name.
    fs::rename(f.root / "alpha" / "beta", f.root / "alpha" / "delta");
    CHECK(run_until(f.loop, [&] {
        return f.view.column_count() == 3 && f.view.column(2).path() == f.root / "alpha" / "delta";
    }));
    CHECK(cursor_name(f.view.column(1)) == "delta");

    // Deleted: the cursor goes, and the column with it.
    fs::remove_all(f.root / "alpha" / "delta");
    CHECK(run_until(f.loop, [&] { return f.view.column_count() == 2; }));
    CHECK_FALSE(f.view.column(1).folder().cursor());
}

TEST_CASE("column view: Shift extends the selection, which closes the next column") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // alpha, opens its column
    REQUIRE(f.view.column_count() == 2);

    f.press(XKB_KEY_Down, {.shift = true});  // alpha, gamma
    Folder& column = f.view.column(0).folder();
    CHECK(column.selected_count() == 2);
    CHECK(cursor_name(column) == "gamma");
    CHECK(f.view.column_count() == 1);  // several entries: nothing to show on the right
    CHECK(column.single_selection() == nullptr);

    f.press(XKB_KEY_End, {.shift = true});  // from the anchor (alpha) to readme.md
    CHECK(column.selected_names() == std::vector<std::string>{"alpha", "gamma", "readme.md"});
    const rummage::views::SelectionSummary& summary = column.summary();
    CHECK(summary.folders == 2);
    CHECK(summary.files == 1);
    CHECK(summary.bytes == 4);  // "# hi"

    f.press(XKB_KEY_Up, {.shift = true});  // the range shrinks back: alpha, gamma
    CHECK(column.selected_names() == std::vector<std::string>{"alpha", "gamma"});

    f.press(XKB_KEY_Escape);  // back to the cursor's entry alone: gamma opens again
    CHECK(column.selected_names() == std::vector<std::string>{"gamma"});
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.column(1).path() == f.root / "gamma");
}

TEST_CASE("column view: Ctrl moves the cursor alone, Ctrl+Space toggles, Ctrl+A selects all") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    Folder& column = f.view.column(0).folder();
    f.press(XKB_KEY_Down);                  // alpha
    f.press(XKB_KEY_Down, {.ctrl = true});  // cursor on gamma, alpha selected
    f.press(XKB_KEY_Down, {.ctrl = true});  // cursor on readme.md
    CHECK(column.selected_names() == std::vector<std::string>{"alpha"});
    CHECK(cursor_name(column) == "readme.md");
    CHECK(f.view.column_count() == 1);  // the cursor is not the selection

    f.press(XKB_KEY_space, {.ctrl = true});  // alpha, readme.md
    CHECK(column.selected_names() == std::vector<std::string>{"alpha", "readme.md"});
    f.press(XKB_KEY_space, {.ctrl = true});  // and off again
    CHECK(column.selected_names() == std::vector<std::string>{"alpha"});
    f.press(XKB_KEY_space);  // plain Space is not selection
    CHECK(column.selected_count() == 1);

    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(column.selected_count() == 3);
    f.press(XKB_KEY_Right);  // several selected: nothing to enter
    CHECK(f.view.focus() == 0);

    f.press(XKB_KEY_Down);  // a plain arrow selects one entry again
    CHECK(column.selected_names() == std::vector<std::string>{"readme.md"});
}

TEST_CASE("column view: Ctrl+Shift+I inverts the selection, which closes the next column") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // alpha, its column open
    REQUIRE(f.view.column_count() == 2);
    f.press(XKB_KEY_I, {.ctrl = true, .shift = true});
    Folder& column = f.view.column(0).folder();
    CHECK(column.selected_names() == std::vector<std::string>{"gamma", "readme.md"});
    CHECK(cursor_name(column) == "alpha");
    CHECK(f.view.column_count() == 1);
}

TEST_CASE("column: toggling and ranges by row, selection follows renames and deletions") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    Folder& column = f.view.column(0).folder();
    column.set_cursor(0, SelectMode::toggle);  // alpha
    column.set_cursor(2, SelectMode::toggle);  // readme.md
    CHECK(column.selected_names() == std::vector<std::string>{"alpha", "readme.md"});
    column.set_cursor(1, SelectMode::extend);  // anchor readme.md -> gamma
    CHECK(column.selected_names() == std::vector<std::string>{"gamma", "readme.md"});

    fs::rename(f.root / "readme.md", f.root / "zz.md");
    CHECK(run_until(f.loop, [&] { return column.is_selected("zz.md"); }));
    CHECK(column.selected_count() == 2);

    fs::remove_all(f.root / "gamma");
    CHECK(run_until(f.loop, [&] { return !column.is_selected("gamma"); }));
    CHECK(column.selected_names() == std::vector<std::string>{"zz.md"});
}

TEST_CASE("grid: shows the focused column's folder, cursor and selection included") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // alpha, open in a second column
    REQUIRE(f.view.column_count() == 2);
    f.press(XKB_KEY_Down, {.shift = true});  // alpha, gamma
    const std::shared_ptr<Folder> folder = f.view.focused_folder();

    f.press(XKB_KEY_2, {.ctrl = true});
    REQUIRE(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.grid.folder() == folder);  // the same object: nothing reloads
    CHECK(cursor_name(*folder) == "gamma");
    CHECK(folder->selected_names() == std::vector<std::string>{"alpha", "gamma"});

    // Before the first draw the grid has one cell per row: Right and Down both step by one.
    f.press(XKB_KEY_Right);  // readme.md, alone
    CHECK(folder->selected_names() == std::vector<std::string>{"readme.md"});
    f.press(XKB_KEY_Home);
    f.press(XKB_KEY_Right, {.shift = true});  // alpha, gamma
    f.press(XKB_KEY_Escape);                  // gamma
    CHECK(folder->selected_names() == std::vector<std::string>{"gamma"});

    // Back to the columns: the folder's column is focused again and opens gamma after it.
    f.press(XKB_KEY_1, {.ctrl = true});
    REQUIRE(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.view.focus() == 0);
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.column(1).path() == f.root / "gamma");
}

TEST_CASE("grid: Enter opens a folder, Backspace the parent; the columns follow") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_2, {.ctrl = true});
    f.press(XKB_KEY_Down);  // alpha
    f.press(XKB_KEY_Return);
    REQUIRE(f.settle());
    REQUIRE(f.grid.folder() != nullptr);
    CHECK(f.grid.folder()->path() == f.root / "alpha");
    CHECK_FALSE(f.grid.folder()->cursor());  // a folder opens with no cursor

    f.press(XKB_KEY_Return);  // nothing under the cursor: stays
    CHECK(f.grid.folder()->path() == f.root / "alpha");
    f.press(XKB_KEY_Down);    // beta
    f.press(XKB_KEY_Return);  // into beta
    REQUIRE(f.settle());
    CHECK(f.grid.folder()->path() == f.root / "alpha" / "beta");
    f.press(XKB_KEY_Down);    // photo.jpg
    f.press(XKB_KEY_Return);  // a file: nothing to open (yet)
    CHECK(f.grid.folder()->path() == f.root / "alpha" / "beta");

    f.press(XKB_KEY_BackSpace);  // back to alpha, cursor on beta
    REQUIRE(f.settle());
    const std::shared_ptr<Folder> alpha = f.grid.folder();
    CHECK(alpha->path() == f.root / "alpha");
    CHECK(cursor_name(*alpha) == "beta");
    f.press(XKB_KEY_Down, {.shift = true});  // beta, notes.txt

    // The columns unfold from root down to the grid's folder, which becomes a column as it is.
    f.press(XKB_KEY_1, {.ctrl = true});
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 2);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(&f.view.column(1).folder() == alpha.get());
    CHECK(f.view.focus() == 1);
    CHECK(alpha->selected_names() == std::vector<std::string>{"beta", "notes.txt"});

    // And the column view works on it as usual.
    f.press(XKB_KEY_Up);  // beta alone: opens in a third column
    REQUIRE(f.view.column_count() == 3);
    CHECK(f.view.column(2).path() == f.root / "alpha" / "beta");
}

TEST_CASE("browser: Enter on a file hands the selected files to the open handler") {
    Fixture f;
    std::vector<std::pair<fs::path, std::vector<std::string>>> opened;
    f.browser.set_open_handler(
        [&](const fs::path& directory, std::vector<rummage::fs::Entry> files) {
            std::vector<std::string> names;
            for (const rummage::fs::Entry& file : files) {
                CHECK_FALSE(file.is_directory());
                CHECK(file.mime != rummage::fs::kUnknownMime);  // the type the view knows
                names.push_back(file.name);
            }
            opened.emplace_back(directory, std::move(names));
        });
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // alpha: a folder goes on to the view
    REQUIRE(f.settle());
    f.press(XKB_KEY_Return);
    CHECK(opened.empty());
    CHECK(f.view.focus() == 1);
    REQUIRE(f.settle());

    f.press(XKB_KEY_Down);  // notes.txt
    f.press(XKB_KEY_Return);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == f.root / "alpha");
    CHECK(opened[0].second == std::vector<std::string>{"notes.txt"});

    // Folders in the selection stay out; the files go together, in listing order.
    f.press(XKB_KEY_Up, {.shift = true});  // beta, notes.txt; the cursor on beta
    f.press(XKB_KEY_Return);               // on a folder: not opened
    CHECK(opened.size() == 1);
    f.press(XKB_KEY_Left);
    f.press(XKB_KEY_End);  // readme.md
    // A type the name leaves open (shared-mime-info 2.5 also gives *.md to Sega ROMs) is sniffed
    // from the content as the rows are drawn; nothing draws here, so ask as a draw would.
    const std::shared_ptr<Folder> top = f.view.focused_folder();
    REQUIRE(top != nullptr);
    top->sniff(0, top->entries().size());
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::none_of(top->entries(), [&](const rummage::fs::Entry& entry) {
            return !entry.is_directory() && top->mime_of(entry) == rummage::fs::kUnknownMime;
        });
    }));
    f.press(XKB_KEY_2, {.ctrl = true});
    f.press(XKB_KEY_a, {.ctrl = true});  // everything; the cursor stays on readme.md
    f.press(XKB_KEY_KP_Enter);
    REQUIRE(opened.size() == 2);
    CHECK(opened[1].first == f.root);
    CHECK(opened[1].second == std::vector<std::string>{"readme.md"});

    // A cursor outside the selection opens only itself.
    f.press(XKB_KEY_Home, {.ctrl = true});     // alpha, the cursor moves, the selection stays
    f.press(XKB_KEY_End, {.ctrl = true});      // back to readme.md, still selected
    f.press(XKB_KEY_Escape);                   // readme.md alone
    f.press(XKB_KEY_Return, {.shift = true});  // with a modifier: not Enter
    CHECK(opened.size() == 2);
}

TEST_CASE("grid: from a folder above the columns, the columns start over from its parent") {
    Fixture f;
    f.view.open(f.root / "alpha");
    REQUIRE(f.settle());
    f.press(XKB_KEY_2, {.ctrl = true});
    f.press(XKB_KEY_BackSpace);  // root, cursor on alpha
    REQUIRE(f.settle());
    f.press(XKB_KEY_End);  // readme.md
    const std::shared_ptr<Folder> root = f.grid.folder();
    f.press(XKB_KEY_1, {.ctrl = true});
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 2);
    CHECK(f.view.column(0).path() == f.root.parent_path());
    CHECK(cursor_name(f.view.column(0)) == "root");
    CHECK(&f.view.column(1).folder() == root.get());
    CHECK(f.view.focus() == 1);
    CHECK(cursor_name(f.view.column(1)) == "readme.md");
}

TEST_CASE("grid: Ctrl+plus, Ctrl+minus and Ctrl+0 change the cell size within its limits") {
    Fixture f;
    f.view.open(f.root);
    REQUIRE(f.settle());
    f.press(XKB_KEY_2, {.ctrl = true});
    const auto standard = rummage::views::kGridIconSizes.at(rummage::views::kUsualGridSize);
    CHECK(f.grid.icon_size() == standard);
    f.press(XKB_KEY_plus, {.ctrl = true});
    CHECK(f.grid.icon_size() > standard);
    for (int i = 0; i < 10; ++i) {
        f.press(XKB_KEY_minus, {.ctrl = true});
    }
    CHECK(f.grid.icon_size() == rummage::views::kGridIconSizes.front());
    for (int i = 0; i < 10; ++i) {
        f.press(XKB_KEY_KP_Add, {.ctrl = true});
    }
    CHECK(f.grid.icon_size() == rummage::views::kGridIconSizes.back());
    f.press(XKB_KEY_0, {.ctrl = true});
    CHECK(f.grid.icon_size() == standard);
    f.press(XKB_KEY_plus);  // without Ctrl: nothing
    CHECK(f.grid.icon_size() == standard);
}

TEST_CASE("column view: revealing a filtered leaf keeps its nonmatching ancestors available") {
    Fixture f;
    f.browser.set_name_filter("photo");
    f.view.reveal(f.root, f.root / "alpha" / "beta" / "photo.jpg");
    REQUIRE(f.settle());
    REQUIRE(f.view.column_count() == 3);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(cursor_name(f.view.column(1)) == "beta");
    CHECK(cursor_name(f.view.column(2)) == "photo.jpg");
    CHECK(f.view.column(0).folder().entries().size() == 3);
    CHECK(f.view.column(1).folder().entries().size() == 2);
}

TEST_CASE("column view: wildcard filters match hidden extensions only in the last column") {
    Fixture f;
    write(f.root / "alpha" / "beta" / "icon.svg", "<svg/>");
    f.view.reveal(f.root, f.root / "alpha" / "beta" / "photo.jpg");
    REQUIRE(f.settle());
    f.browser.set_show_extensions(false);
    f.browser.set_name_filter("*.svg");
    REQUIRE(f.view.column_count() == 3);
    CHECK(cursor_name(f.view.column(0)) == "alpha");
    CHECK(cursor_name(f.view.column(1)) == "beta");
    const auto& entries = f.view.column(2).folder().entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().name == "icon.svg");
    f.browser.set_name_filter("svg");
    REQUIRE(f.view.column_count() == 3);
    REQUIRE(f.view.column(2).folder().entries().size() == 1);
    CHECK(f.view.column(2).folder().entries().front().name == "icon.svg");
}

TEST_CASE("collection folder: duplicate filenames retain independent stable selections") {
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder = std::make_shared<Folder>(loop, pool, watcher, mime, [] {});
    rummage::fs::Entry first{.name = "photo.jpg",
                             .kind = rummage::fs::EntryKind::file,
                             .size = 10,
                             .identity = "photo:1",
                             .location = "/unmounted/a/photo.jpg",
                             .available = false};
    auto second = first;
    second.identity = "photo:2";
    second.location = "/unmounted/b/photo.jpg";
    second.size = 20;
    folder->replace_collection({first, second}, "Photos", 1000);
    CHECK(folder->is_collection());
    CHECK(folder->path().empty());
    CHECK_FALSE(folder->model().loading());
    CHECK(folder->model().path().empty());
    CHECK(folder->total_count() == 1000);
    CHECK(folder->entries()[0].name == folder->entries()[1].name);
    folder->set_cursor(0);
    folder->set_cursor(1, SelectMode::toggle);
    CHECK(folder->selected_count() == 2);
    CHECK(folder->selected_names() == std::vector<std::string>{"photo:1", "photo:2"});
    CHECK(folder->summary().bytes == 30);
    CHECK(folder->selected_paths() == std::vector<fs::path>{first.location, second.location});
    folder->replace_collection({second, first}, "Photos", 1000);
    REQUIRE(folder->cursor_entry());
    CHECK(folder->cursor_entry()->key() == "photo:2");
    CHECK(folder->cursor() == 0);
    CHECK(folder->selected_count() == 2);
    folder->set_order({.key = rummage::fs::SortKey::size});
    CHECK(folder->entries()[0].key() == "photo:2");  // controller owns full-query sorting
    folder->select_name("photo:1");
    REQUIRE(folder->single_selection());
    CHECK(folder->single_selection()->location == first.location);
    folder->sniff(0, 2);  // offline rows never schedule original reads
    folder->replace_collection({second}, "Photos", 999);
    CHECK(folder->cursor_entry() == nullptr);
    CHECK(folder->selected_count() == 0);
    rummage::fs::Entry logical{.name = "Sources",
                               .kind = rummage::fs::EntryKind::other,
                               .identity = "sources",
                               .available = false};
    folder->replace_collection({logical}, "Library", 1);
    folder->select_all();
    CHECK(folder->selected_paths().empty());
    CHECK(folder->selected_entries().size() == 1);
}

TEST_CASE("collection folder: shared views adopt rows without opening a directory") {
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder = std::make_shared<Folder>(loop, pool, watcher, mime, [] {});
    rummage::fs::Entry row{.name = "Duplicate name",
                           .kind = rummage::fs::EntryKind::other,
                           .identity = "source:1",
                           .available = false};
    folder->replace_collection({row}, "Sources", 1, true);
    ColumnView columns{loop, pool, watcher, mime, [] {}, [](rummage::ui::CursorShape) {}};
    GridView grid{loop, pool, watcher, mime, [] {}, [](rummage::ui::CursorShape) {}};
    rummage::views::DetailsView details{loop, pool,  watcher,
                                        mime, [] {}, [](rummage::ui::CursorShape) {}};
    columns.show(folder);
    grid.show(folder);
    details.show(folder);
    CHECK(columns.focused_folder() == folder);
    CHECK(grid.folder() == folder);
    CHECK(details.folder() == folder);
    CHECK(columns.loading());
    CHECK(grid.loading());
    CHECK(details.loading());
    folder->replace_collection({row}, "Sources", 1);
    folder->select_name(row.key());
    CHECK_FALSE(columns.loading());
    CHECK_FALSE(grid.loading());
    CHECK_FALSE(details.loading());
    CHECK(folder->model().path().empty());
    CHECK(folder->path_of(row).empty());
}

TEST_CASE("gallery: native view menu selects Grid and preserves the window preference") {
    Fixture f;
    f.browser.open(f.root);
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::details);
    CHECK_FALSE(f.browser.gallery());
    f.browser.open_view_menu(0, 0, true);
    const auto& items = f.browser.context_menu().items();
    const auto found = std::ranges::find(items, "gallery", &rummage::views::MenuItem::id);
    REQUIRE(found != items.end());
    CHECK(found->checkable);
    CHECK_FALSE(found->checked);
    auto activate = found->action;
    f.browser.context_menu().close();
    activate();
    CHECK(f.browser.gallery());
    CHECK(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.state().gallery);
    f.browser.set_mode(Browser::Mode::columns);
    CHECK(f.browser.gallery());
    f.browser.open_view_menu(0, 0, true);
    const auto& reopened = f.browser.context_menu().items();
    const auto enabled = std::ranges::find(reopened, "gallery", &rummage::views::MenuItem::id);
    REQUIRE(enabled != reopened.end());
    CHECK(enabled->checked);
    activate = enabled->action;
    f.browser.context_menu().close();
    activate();
    CHECK_FALSE(f.browser.gallery());
    CHECK(f.browser.mode() == Browser::Mode::grid);
}

TEST_CASE("gallery: offline cached media hides names only until focused or hovered") {
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(400, 400);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    TempDir directory;
    const auto photo = directory.path / "same.jpg";
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "image/with-thumbnail.jpg", photo);
    struct stat info{};
    REQUIRE(::stat(photo.c_str(), &info) == 0);
    auto cache = std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "gallery-test");
    REQUIRE_FALSE(cache->get(photo, rummage::thumbs::Size::normal).image.empty());
    fs::remove(photo);
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    rummage::views::Thumbnails thumbnails{loop, pool, cache, [] {}};
    auto folder = std::make_shared<Folder>(loop, pool, watcher, mime, [] {});
    rummage::fs::Entry first;
    first.name = "same.jpg";
    first.kind = rummage::fs::EntryKind::file;
    first.mime = mime->by_name(first.name).mime;
    first.modified_ns =
        static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec;
    first.identity = "photo:100";
    first.location = photo;
    first.available = false;
    auto missing = first;
    missing.identity = "photo:101";
    missing.location = directory.path / "missing" / "same.jpg";
    rummage::fs::Entry logical;
    logical.name = "Album";
    logical.kind = rummage::fs::EntryKind::directory;
    logical.identity = "album:1";
    logical.available = false;
    folder->replace_collection({first, missing, logical}, "Photos", 3);
    folder->set_cursor(std::nullopt);
    GridView grid{loop, pool, watcher, mime, [] {}, [](rummage::ui::CursorShape) {}, &thumbnails};
    grid.set_info_visible(false);
    grid.set_size_index(Browser::grid_index(96));
    grid.set_gallery(true);
    grid.show(folder);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(400, 400, 1, {});
        grid.draw(renderer, {0, 0, 400, 400}, rummage::theme::Palette{}, icons,
                  GridView::Clock::now());
        renderer.end_frame();
    };
    draw();
    REQUIRE(run_until(loop, [&] { return !thumbnails.loading(); }));
    draw();
    REQUIRE(grid.columns() == 2);
    const auto gallery_hit = grid.hit(21, 21);
    REQUIRE(gallery_hit);
    CHECK(gallery_hit->entry == 0);
    CHECK(gallery_hit->on_item);  // The thumbnail's enlarged gallery cell is interactive.
    const auto missing_hit = grid.hit(205, 21);
    REQUIRE(missing_hit);
    CHECK_FALSE(missing_hit->on_item);  // Uncached originals keep the normal labelled cell.
    const auto folder_hit = grid.hit(21, 21 + grid.cell_height());
    REQUIRE(folder_hit);
    CHECK_FALSE(folder_hit->on_item);
    folder->set_cursor(0);
    draw();
    CHECK_FALSE(grid.hit(21, 21)->on_item);
    folder->set_cursor(std::nullopt);
    grid.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 100, .y = 60},
                 GridView::Clock::now());
    draw();
    CHECK_FALSE(grid.hit(21, 21)->on_item);
    grid.pointer({.kind = rummage::ui::PointerEvent::Kind::leave}, GridView::Clock::now());
    grid.set_show_thumbnails(false);
    draw();
    CHECK_FALSE(grid.hit(21, 21)->on_item);
    grid.set_show_thumbnails(true);
    grid.set_gallery(false);
    draw();
    CHECK_FALSE(grid.hit(21, 21)->on_item);
    CHECK_FALSE(fs::exists(photo));
    CHECK_FALSE(fs::exists(missing.location.parent_path()));
}

TEST_CASE("collection viewport: notifications clamp ranges and allow handler replacement") {
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    Folder folder{loop, pool, watcher, mime, [] {}};
    folder.replace_collection(std::vector<rummage::fs::Entry>(3), "Photos", 300);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    folder.set_visible_range_handler([&](std::size_t first, std::size_t last) {
        ranges.emplace_back(first, last);
        folder.set_visible_range_handler({});
        folder.replace_collection({}, "Photos", 300);
    });
    folder.notify_visible_range(1, 200);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0] == std::pair<std::size_t, std::size_t>{1, 3});
    folder.notify_visible_range(0, 0);
    CHECK(ranges.size() == 1);
    folder.set_visible_range_handler(
        [&](std::size_t first, std::size_t last) { ranges.emplace_back(first, last); });
    folder.notify_visible_range(10, 2);
    CHECK(ranges.back() == std::pair<std::size_t, std::size_t>{0, 0});

    TempDir directory;
    Folder ordinary{loop, pool, watcher, mime, directory.path, [] {}};
    ordinary.set_visible_range_handler(
        [&](std::size_t, std::size_t) { FAIL("filesystem callback"); });
    ordinary.notify_visible_range(0, 200);
}

TEST_CASE("collection viewport: native views report the drawn range after scrolling") {
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(400, 400);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    auto folder = std::make_shared<Folder>(loop, pool, watcher, mime, [] {});
    std::vector<rummage::fs::Entry> entries;
    for (std::size_t i = 0; i < 200; ++i) {
        rummage::fs::Entry entry;
        entry.name = "Photo " + std::to_string(i);
        entry.identity = "photo:" + std::to_string(i);
        entry.available = false;
        entries.push_back(std::move(entry));
    }
    folder->replace_collection(entries, "Photos", 450);
    GridView grid{loop, pool, watcher, mime, [] {}, [](rummage::ui::CursorShape) {}};
    rummage::views::DetailsView details{loop, pool,  watcher,
                                        mime, [] {}, [](rummage::ui::CursorShape) {}};
    ColumnView columns{loop, pool, watcher, mime, [] {}, [](rummage::ui::CursorShape) {}};
    grid.set_info_visible(false);
    details.set_info_visible(false);
    columns.set_info_visible(false);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    bool append = false;
    folder->set_visible_range_handler([&](std::size_t first, std::size_t last) {
        ranges.emplace_back(first, last);
        if (append) {
            append = false;
            auto more = entries;
            auto extra = entries.back();
            extra.identity = "photo:200";
            more.push_back(std::move(extra));
            folder->replace_collection(std::move(more), "Photos", 450);
        }
    });
    const auto exercise = [&](auto& view, auto& scroller) {
        const auto draw = [&] {
            renderer.begin_frame(400, 400, 1, {});
            view.draw(renderer, {0, 0, 400, 400}, rummage::theme::Palette{}, icons,
                      GridView::Clock::now());
            renderer.end_frame();
        };
        draw();
        REQUIRE_FALSE(ranges.empty());
        CHECK(ranges.back().first == 0);
        CHECK(ranges.back().second > 0);
        CHECK(ranges.back().second < 200);
        scroller.jump_to(100000);
        append = true;
        draw();
        CHECK(ranges.back().first > 0);
        CHECK(ranges.back().second == 200);
        CHECK(folder->entries().size() == 201);
        CHECK(folder->path().empty());
    };
    SUBCASE("Grid") {
        grid.show(folder);
        exercise(grid, grid.scroller());
    }
    SUBCASE("Details") {
        details.show(folder);
        exercise(details, details.scroller());
    }
    SUBCASE("Columns") {
        columns.show(folder);
        REQUIRE(columns.column_count() == 1);
        exercise(columns, columns.column(0).scroller());
    }
}

TEST_CASE("collection folder: incremental metadata and append preserve stable selection") {
    EventLoop loop;
    WorkerPool pool{loop, 1};
    DirectoryWatcher watcher{loop};
    Folder folder{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    rummage::fs::Entry first{.name = "photo.jpg", .size = 10, .identity = "photo:1"};
    auto second = first;
    second.identity = "photo:2";
    folder.replace_collection({first, second}, "Photos", 3);
    folder.set_cursor(1);
    const auto selection_revision = folder.selection_revision();
    CHECK(folder.summary().bytes == 10);
    second.size = 42;
    CHECK(folder.update_collection_rows({{1, second}}));
    CHECK(folder.cursor_entry()->key() == "photo:2");
    CHECK(folder.summary().bytes == 42);
    CHECK(folder.selection_revision() == selection_revision);
    auto invalid = first;
    invalid.identity = "wrong-photo";
    first.size = 99;
    CHECK_FALSE(folder.update_collection_rows({{0, first}, {1, invalid}}));
    CHECK(folder.entries()[0].size == 10);  // invalid patches fail before any mutation
    CHECK(folder.entries()[1].key() == "photo:2");
    auto third = first;
    third.identity = "photo:3";
    folder.append_collection({third}, 3);
    CHECK(folder.entries().size() == 3);
    CHECK(folder.cursor_entry()->key() == "photo:2");
    CHECK(folder.selected_names() == std::vector<std::string>{"photo:2"});
    CHECK(folder.selection_revision() == selection_revision);
}
