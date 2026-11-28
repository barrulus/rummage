#include "rummage/views/undo.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
#include "rummage/views/conflict_dialog.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::FileOperation;
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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

Names listing(const fs::path& directory) {
    Names names;
    std::error_code ignored;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

const Modifiers kCtrl{.ctrl = true};
const Modifiers kCtrlShift{.ctrl = true, .shift = true};

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/}, a trash of its own.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path photos = root / "photos";
    fs::path album = root / "album";
    fs::path trash = dir.path / "Trash";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    explicit Fixture(const fs::path& folder = "root") : root{dir.path / folder} {
        fs::create_directories(photos);
        fs::create_directories(album);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P2.ORF"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(trash);
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    // Presses event and waits for what it started.
    void run(const KeyEvent& event) {
        CHECK(press(event));
        REQUIRE(settle());
    }

    std::shared_ptr<Folder> show(const fs::path& folder, const std::string& cursor_on) {
        browser.open(folder);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
        std::shared_ptr<Folder> shown = browser.details().folder();
        if (!cursor_on.empty()) {
            shown->select_name(cursor_on);
            REQUIRE(run_until(loop, [&] { return shown->cursor_entry() != nullptr; }));
        }
        return shown;
    }

    [[nodiscard]] std::string message() { return browser.status_bar().message(); }
};

}  // namespace

TEST_CASE("undo: a rename, sidecars along; redo; then nothing left to undo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    CHECK(f.press(key(XKB_KEY_NoSymbol, {}, "Lake")));
    f.run(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.name_editor().active(); }));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “Lake.ORF” is “P1.ORF” again");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "Nothing to undo");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Redone: “P1.ORF” is “Lake.ORF” again");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(f.message() == "Nothing to redo");
    f.run(key(XKB_KEY_z, kCtrl));  // the redo is undone like any rename
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("undo: a move goes back, and comes again on redo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P1.ORF” back in photos");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.message() == "Redone: “P1.ORF” back in album");
}

TEST_CASE("undo: copies go to the trash, never deleted; redo brings them back") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});  // the originals stay
    CHECK(f.message() == "Undone: “P1.ORF” moved to the trash");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.trash / "files").empty());
    CHECK(f.message() == "Redone: “P1.ORF” restored");
}

TEST_CASE("undo: after a merge only what the copy made goes; what was there stays") {
    Fixture f;
    // album/photos is there already, with a file of its own and an older P2.ORF.
    fs::create_directories(f.album / "photos");
    std::ofstream{f.album / "photos" / "mine.txt"} << "mine";
    std::ofstream{f.album / "photos" / "P2.ORF"} << "album's P2";
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));  // merge the folders
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_r)));  // replace the older P2.ORF
    REQUIRE(f.settle());
    REQUIRE(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});

    f.run(key(XKB_KEY_z, kCtrl));
    // The new files go to the trash; the folder and what it held before stay.
    CHECK(listing(f.album / "photos") == Names{"P2.ORF", "mine.txt"});
    CHECK(listing(f.trash / "files") == Names{"P1.ORF", "P1.ORF.xmp"});
}

TEST_CASE("undo: a move merged into a folder goes back into its own, made again") {
    Fixture f;
    fs::create_directories(f.album / "photos");
    std::ofstream{f.album / "photos" / "mine.txt"} << "mine";
    std::ofstream{f.album / "photos" / "P2.ORF"} << "album's P2";
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));  // merge
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_s)));  // keep album's P2.ORF: photos/P2.ORF stays behind
    REQUIRE(f.settle());
    REQUIRE(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});
    REQUIRE(listing(f.photos) == Names{"P2.ORF"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.album / "photos") == Names{"P2.ORF", "mine.txt"});
    CHECK(f.message() == "Undone: “P1.ORF” back in photos");

    // All of it moved this time: the emptied photos folder is gone, and made again by the undo.
    fs::remove(f.album / "photos" / "P2.ORF");
    f.show(f.root, "photos");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE_FALSE(fs::exists(f.photos));
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.album / "photos") == Names{"mine.txt"});
}

TEST_CASE("undo: replaced files are said, never brought back") {
    Fixture f;
    std::ofstream{f.album / "P2.ORF"} << "album's P2";
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    f.show(f.photos, "P2.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"P2.ORF"});

    CHECK(f.press(key(XKB_KEY_z, kCtrl)));  // only a replaced file: nothing to bring back
    CHECK(f.message() == "1 item cannot be safely undone");
    CHECK(listing(f.album) == Names{"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_z, kCtrl)));
    CHECK(f.message() == "Nothing to undo");

    // Everything in photos: P1.ORF is new, P2.ORF replaces album's again.
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_a, kCtrl)));
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    CHECK(dialog.conflict().name == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album) == Names{"P2.ORF"});
    CHECK(f.message() == "Undone: “P1.ORF” moved to the trash; 1 item cannot be safely undone");
}

TEST_CASE("undo: Delete is undone by restoring; a permanent delete cannot be") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    f.run(key(XKB_KEY_Delete));
    REQUIRE(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P2.ORF” restored");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
    f.run(key(XKB_KEY_Return));
    REQUIRE(listing(f.photos).empty());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "A permanent delete cannot be undone");
}

TEST_CASE("undo: a new folder goes to the trash while it is empty") {
    Fixture f;
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_n, kCtrlShift)));
    REQUIRE(run_until(f.loop, [&] { return f.browser.name_editor().active(); }));
    CHECK(f.press(key(XKB_KEY_Escape)));  // keeps "New Folder"
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"New Folder"});

    // Something put in it: not taken away.
    std::ofstream{f.album / "New Folder" / "note.txt"} << "mine";
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "“New Folder” has changed since it was made: not undone");
    CHECK(listing(f.album) == Names{"New Folder"});

    fs::remove(f.album / "New Folder" / "note.txt");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"New Folder"});
}

TEST_CASE("undo: what changed meanwhile is said, and undo goes on working") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P2.ORF"});
    fs::remove(f.album / "P2.ORF");  // another program took it away

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message().find("P2.ORF") != std::string::npos);
    CHECK(f.message().find("could not be") != std::string::npos);
    // Nothing came back: nothing to redo, and Ctrl+Z still answers.
    CHECK_FALSE(f.browser.undone_operation().has_value());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() != "Still undoing the last one");
}

TEST_CASE("undo: what the messages say") {
    using Kind = FileOperation::Kind;
    CHECK(rummage::views::describe({.kind = Kind::trash,
                                    .items = {{.from = "/a/x.txt", .to = "/T/files/x.2.txt"},
                                              {.from = "/a/y.txt", .to = "/T/files/y.txt"}},
                                    .modes = {}}) == "2 items moved to the trash");
    CHECK(rummage::views::describe({.kind = Kind::move,
                                    .items = {{.from = "/a/x.txt", .to = "/b/x.txt"}},
                                    .modes = {}}) == "“x.txt” back in b");
}

TEST_CASE("undo: dual-pane merged transfers preserve the destination through undo and redo") {
    Fixture f;
    bool move = false;
    SUBCASE("copy") {}
    SUBCASE("move") { move = true; }
    fs::create_directories(f.album / "photos");
    std::ofstream{f.album / "photos" / "mine.txt"} << "mine";
    f.show(f.root, "photos");
    f.browser.toggle_split();
    f.show(f.album, "");
    f.browser.focus_pane(0);
    f.browser.details().folder()->select_name("photos");
    f.browser.transfer_to_other_pane(move);
    auto& dialog = f.browser.conflict_dialog();
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    REQUIRE(dialog.conflict().directory);
    REQUIRE(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});
    CHECK(fs::exists(f.photos) == !move);

    // Undo belongs to the browser, even when the receiving pane has focus.
    f.browser.focus_pane(1);
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album / "photos") == Names{"mine.txt"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "mine.txt"});
    // A merge records its leaf items. Redo moves those again; it leaves the source folder
    // recreated by undo in place rather than claiming that folder as one of its own items.
    if (move) {
        CHECK(listing(f.photos).empty());
    } else {
        CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    }
}

TEST_CASE("undo: kept-both moves restore their original names and sidecars, then redo") {
    Fixture f;
    bool merge = false;
    SUBCASE("top-level file") {}
    SUBCASE("file inside a merged folder") { merge = true; }
    const fs::path destination = merge ? f.album / "photos" : f.album;
    fs::create_directories(destination);
    std::ofstream{destination / "P1.ORF"} << "destination photo";
    std::ofstream{destination / "P1.ORF.xmp"} << "destination sidecar";
    f.show(merge ? f.root : f.photos, merge ? "photos" : "P1.ORF");
    REQUIRE(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    REQUIRE(f.press(key(XKB_KEY_v, kCtrl)));
    auto& dialog = f.browser.conflict_dialog();
    REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    if (merge) {
        REQUIRE(dialog.conflict().directory);
        REQUIRE(f.press(key(XKB_KEY_r)));
        REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
    }
    REQUIRE(dialog.conflict().name == "P1.ORF");
    REQUIRE(f.press(key(XKB_KEY_Return)));  // Keep Both
    REQUIRE(f.settle());
    REQUIRE(fs::exists(destination / "P1 2.ORF"));
    REQUIRE(fs::exists(destination / "P1 2.ORF.xmp"));
    CHECK_FALSE(fs::exists(f.photos / "P1.ORF"));

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(destination) == Names{"P1.ORF", "P1.ORF.xmp"});
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(fs::exists(destination / "P1 2.ORF"));
    CHECK(fs::exists(destination / "P1 2.ORF.xmp"));
    CHECK_FALSE(fs::exists(f.photos / "P1.ORF"));
    CHECK_FALSE(fs::exists(f.photos / "P1.ORF.xmp"));
    const auto read = [](const fs::path& file) {
        std::ifstream stream{file};
        return std::string{std::istreambuf_iterator<char>{stream}, {}};
    };
    CHECK(read(destination / "P1.ORF") == "destination photo");
    CHECK(read(destination / "P1.ORF.xmp") == "destination sidecar");
    CHECK(read(destination / "P1 2.ORF") == "P1.ORF");
    CHECK(read(destination / "P1 2.ORF.xmp") == "P1.ORF.xmp");
}

TEST_CASE("undo: a preexisting destination sidecar keeps its photo group out of undo") {
    Fixture f;
    bool move = false;
    SUBCASE("copy") {}
    SUBCASE("move") { move = true; }
    std::ofstream{f.album / "P2.ORF.xmp"} << "destination's sidecar";
    f.show(f.photos, "P2.ORF");  // P2 has no source sidecar
    REQUIRE(f.press(key(move ? XKB_KEY_x : XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    REQUIRE(f.press(key(XKB_KEY_v, kCtrl)));
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"P2.ORF", "P2.ORF.xmp"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "1 item cannot be safely undone");
    CHECK(listing(f.album) == Names{"P2.ORF", "P2.ORF.xmp"});
    CHECK_FALSE(fs::exists(f.trash / "files" / "P2.ORF.xmp"));
    std::ifstream sidecar{f.album / "P2.ORF.xmp"};
    CHECK(std::string{std::istreambuf_iterator<char>{sidecar}, {}} == "destination's sidecar");
}

TEST_CASE("undo: new links go to the trash, what they lead to stays; redo brings them back") {
    Fixture f;
    std::ofstream{f.album / "kept.txt"} << "kept";
    const std::shared_ptr<Folder> shown = f.show(f.root, "album");
    f.browser.create_links();
    REQUIRE(f.settle());
    REQUIRE(listing(f.root) == Names{"Link to album", "album", "photos"});
    CHECK(fs::read_symlink(f.root / "Link to album") == f.album);
    CHECK(f.message() == "Made 1 link");
    REQUIRE(run_until(f.loop, [&] { return shown->selected_names() == Names{"Link to album"}; }));

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.root) == Names{"album", "photos"});
    CHECK(listing(f.trash / "files") == Names{"Link to album"});
    CHECK(fs::is_symlink(f.trash / "files" / "Link to album"));
    CHECK(listing(f.album) == Names{"kept.txt"});  // what the link led to stays
    CHECK(f.message() == "Undone: “Link to album” moved to the trash");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.root) == Names{"Link to album", "album", "photos"});
    CHECK(fs::read_symlink(f.root / "Link to album") == f.album);
    CHECK(f.message() == "Redone: “Link to album” restored");

    // Several at once, one operation.
    const std::shared_ptr<Folder> photos = f.show(f.photos, "");
    for (const char* name : {"P1.ORF", "P2.ORF"}) {
        const auto& entries = photos->entries();
        const auto it = std::ranges::find(entries, name, &rummage::fs::Entry::name);
        REQUIRE(it != entries.end());
        photos->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                           rummage::views::SelectMode::toggle);
    }
    REQUIRE(photos->selected_names() == Names{"P1.ORF", "P2.ORF"});
    f.browser.create_links();
    REQUIRE(f.settle());
    CHECK(listing(f.photos) ==
          Names{"Link to P1.ORF", "Link to P2.ORF", "P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Made 2 links");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("links: paste a chosen link type into another folder and undo only the new links") {
    for (const auto kind : {rummage::fs::LinkKind::absolute, rummage::fs::LinkKind::relative,
                            rummage::fs::LinkKind::hard}) {
        Fixture f;
        f.show(f.photos, "P2.ORF");
        f.run(key(XKB_KEY_c, kCtrl));
        f.show(f.album, "");
        f.browser.paste_links(kind);
        REQUIRE(f.settle());
        REQUIRE(fs::exists(f.album / "P2.ORF"));
        CHECK(fs::equivalent(f.album / "P2.ORF", f.photos / "P2.ORF"));
        if (kind == rummage::fs::LinkKind::hard)
            CHECK_FALSE(fs::is_symlink(f.album / "P2.ORF"));
        else
            CHECK(fs::read_symlink(f.album / "P2.ORF").is_absolute() ==
                  (kind == rummage::fs::LinkKind::absolute));
        f.browser.undo();
        REQUIRE(f.settle());
        CHECK_FALSE(fs::exists(f.album / "P2.ORF"));
        CHECK(fs::exists(f.photos / "P2.ORF"));
        f.browser.redo();
        REQUIRE(f.settle());
        CHECK(fs::equivalent(f.album / "P2.ORF", f.photos / "P2.ORF"));
    }
}

TEST_CASE("undo: move into a new folder is one operation with sidecars and redo") {
    Fixture f;
    auto shown = f.show(f.photos, "P1.ORF");
    f.browser.move_into_new_folder();
    REQUIRE(f.browser.confirm_dialog().is_open());
    f.browser.confirm_dialog().paste("Selected");
    f.run(key(XKB_KEY_Return));
    CHECK(listing(f.photos) == Names{"P2.ORF", "Selected"});
    CHECK(listing(f.photos / "Selected") == Names{"P1.ORF", "P1.ORF.xmp"});
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    f.browser.redo();
    REQUIRE(f.settle());
    CHECK(listing(f.photos / "Selected") == Names{"P1.ORF", "P1.ORF.xmp"});
    std::ofstream{f.photos / "Selected" / "added.txt"} << "keep";
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(listing(f.photos / "Selected") == Names{"added.txt"});
    CHECK(fs::exists(f.photos / "P1.ORF.xmp"));
    f.browser.redo();
    REQUIRE(f.settle());
    CHECK(listing(f.photos / "Selected") == Names{"P1.ORF", "P1.ORF.xmp", "added.txt"});
}

TEST_CASE("transfer tasks: queued cancellation preserves another job's conflict and undo") {
    Fixture f;
    std::ofstream{f.album / "P1.ORF"} << "old";
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    f.browser.paste();
    REQUIRE(run_until(f.loop, [&] { return f.browser.conflict_dialog().is_open(); }));
    f.browser.paste();
    const auto tasks = f.browser.transfer_tasks();
    REQUIRE(tasks.size() == 2);
    f.browser.cancel_transfer_task(tasks.back().id);
    REQUIRE(run_until(f.loop, [&] { return f.browser.transfer_tasks().back().finished(); }));
    CHECK(f.browser.conflict_dialog().is_open());
    CHECK(f.browser.conflict_dialog().conflict().name == "P1.ORF");
    CHECK(f.press(key(XKB_KEY_Return)));  // keep both
    REQUIRE(f.settle());
    REQUIRE(fs::exists(f.album / "P1 2.ORF"));
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK_FALSE(fs::exists(f.album / "P1 2.ORF"));
    CHECK(fs::exists(f.album / "P1.ORF"));
}

TEST_CASE("transfer tasks: palette filters tasks and exposes completed details and clear") {
    // Paths are searched too: make the task match the clear action's query deterministically.
    Fixture f{"Clear Finished Tasks"};
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    f.browser.open_transfers();
    auto& palette = f.browser.command_palette();
    REQUIRE(palette.external());
    palette.set_query("P1.ORF");
    REQUIRE(palette.items().size() == 1);
    CHECK(palette.items().front().label.find("Completed") != std::string::npos);
    palette.choose_highlighted();
    REQUIRE(palette.external());
    CHECK(palette.status().find("Completed") != std::string::npos);
    CHECK(std::ranges::none_of(palette.items(), [](const auto& item) {
        return item.id == "task:cancel" || item.id == "task:pause";
    }));
    palette.set_query("Source:");
    REQUIRE(palette.items().size() == 1);
    CHECK(palette.items().front().label.find(f.photos.string()) != std::string::npos);
    palette.choose_highlighted();  // informational rows keep the panel open
    CHECK(palette.external());
    f.browser.open_transfers();
    palette.set_query("Clear Finished Tasks");
    const auto clear = std::ranges::find(palette.items(), "tasks:clear",
                                        &rummage::views::PaletteItem::id);
    REQUIRE(clear != palette.items().end());
    REQUIRE(palette.items().size() == 2);  // The completed transfer's path also matches.
    const auto index = static_cast<std::size_t>(clear - palette.items().begin());
    const auto row = std::ranges::find(palette.shown(), index);
    REQUIRE(row != palette.shown().end());
    palette.key(key(XKB_KEY_Home));
    for (auto at = palette.shown().begin(); at != row; ++at)
        palette.key(key(XKB_KEY_Down));
    REQUIRE(palette.highlighted());
    CHECK(palette.shown()[*palette.highlighted()] == index);
    palette.choose_highlighted();
    CHECK(palette.external());
    CHECK(palette.status() == "No transfers yet");
    CHECK(f.browser.transfer_tasks().empty());
    palette.close();
    f.run(key(XKB_KEY_z, kCtrl));  // clearing visible history does not clear undo
    CHECK(listing(f.album).empty());
}
