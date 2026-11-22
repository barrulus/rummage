#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_clipboard.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::ClipboardFiles;
using rummage::views::SystemClipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace views = rummage::views;

namespace {

using Names = std::vector<std::string>;
using Paths = std::vector<fs::path>;

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

// The system clipboard as a program would see it: what rummage offered, or another program's.
struct FakeClipboard {
    EventLoop* loop{nullptr};
    std::vector<SystemClipboard::Format> formats;
    bool ours{false};
    int cleared{0};
    std::uint64_t generation{0};  // other contents: one more

    SystemClipboard port() {
        return {.offer =
                    [this](std::vector<SystemClipboard::Format> offered) {
                        formats = std::move(offered);
                        ours = true;
                        ++generation;
                    },
                .clear =
                    [this] {
                        formats.clear();
                        ours = false;
                        ++cleared;
                        ++generation;
                    },
                .owned = [this] { return ours; },
                .generation = [this] { return generation; },
                .types =
                    [this] {
                        std::vector<std::string> types;
                        types.reserve(formats.size());
                        for (const auto& [type, data] : formats) {
                            types.push_back(type);
                        }
                        return types;
                    },
                .read =
                    [this](const std::string& type, SystemClipboard::Done done) {
                        std::optional<std::string> data;
                        for (const auto& [offered, bytes] : formats) {
                            if (offered == type) {
                                data = bytes;
                            }
                        }
                        // Later, as a pipe would answer.
                        loop->post([done = std::move(done), data] { done(data); });
                    }};
    }

    // Another program copies these.
    void copy_from_elsewhere(std::vector<SystemClipboard::Format> offered) {
        formats = std::move(offered);
        ours = false;
        ++generation;
    }

    [[nodiscard]] std::string data(std::string_view type) const {
        for (const auto& [offered, bytes] : formats) {
            if (offered == type) {
                return bytes;
            }
        }
        return {};
    }
};

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/, other/{note.txt}}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    FakeClipboard clipboard{
        .loop = &loop, .formats = {}, .ours = false, .cleared = 0, .generation = 0};

    Fixture() {
        fs::create_directories(root / "photos");
        fs::create_directories(root / "album");
        fs::create_directories(root / "other");
        for (const char* name :
             {"photos/P1.ORF", "photos/P1.ORF.xmp", "photos/P2.ORF", "other/note.txt"}) {
            std::ofstream{root / name} << name;
        }
        browser.set_home_trash(dir.path / "Trash");
        browser.set_system_clipboard(clipboard.port());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    void show(const fs::path& folder, const std::string& cursor_on) {
        browser.open(folder);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
        if (!cursor_on.empty()) {
            browser.details().folder()->select_name(cursor_on);
            REQUIRE(run_until(
                loop, [this] { return browser.details().folder()->cursor_entry() != nullptr; }));
        }
    }
};

}  // namespace

TEST_CASE("clipboard: file URIs as GLib writes them, lists as other programs write them") {
    const Paths paths{"/home/me/a b%#?.ORF", "/home/me/caffè.jpg"};
    CHECK(views::uri_list(paths) ==
          "file:///home/me/a%20b%25%23%3F.ORF\r\nfile:///home/me/caff%C3%A8.jpg\r\n");
    CHECK(views::gnome_copied_files(true, paths) ==
          "cut\nfile:///home/me/a%20b%25%23%3F.ORF\nfile:///home/me/caff%C3%A8.jpg");
    CHECK(views::gnome_copied_files(false, paths).starts_with("copy\n"));
    CHECK(views::path_lines(paths) == "/home/me/a b%#?.ORF\n/home/me/caffè.jpg");

    // Back again, whatever the writer.
    const ClipboardFiles read = views::parse_uri_list(views::uri_list(paths));
    CHECK(read.paths == paths);
    CHECK_FALSE(read.cut);
    const std::optional<ClipboardFiles> gnome =
        views::parse_gnome_copied_files(views::gnome_copied_files(true, paths));
    REQUIRE(gnome.has_value());
    CHECK(gnome.value_or(ClipboardFiles{}).cut);
    CHECK(gnome.value_or(ClipboardFiles{}).paths == paths);
    CHECK_FALSE(views::parse_gnome_copied_files("file:///a\n").has_value());
}

TEST_CASE("clipboard: only local files are taken from a URI list") {
    const ClipboardFiles files = views::parse_uri_list(
        "# a comment\r\n"
        "file:///a/b.txt\r\n"
        "\r\n"
        "file://localhost/c/d/\n"
        "FILE:///e\n"
        "file://otherhost/f\n"
        "https://example.com/g.jpg\n"
        "file:///h%00i\n");
    CHECK(files.paths == Paths{"/a/b.txt", "/c/d", "/e"});
    CHECK(files.refused == 3);
}

TEST_CASE("clipboard: Ctrl+C and Ctrl+X offer the files to other programs") {
    Fixture f;
    f.show(f.root / "photos", "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    const fs::path p1 = f.root / "photos" / "P1.ORF";
    CHECK(f.clipboard.ours);
    CHECK(f.clipboard.data(views::kGnomeFiles) == "copy\n" + rummage::thumbs::file_uri(p1));
    CHECK(f.clipboard.data(views::kUriList) == rummage::thumbs::file_uri(p1) + "\r\n");
    CHECK(f.clipboard.data(views::kText) == p1.string());
    CHECK(f.clipboard.data(views::kKdeCut).empty());

    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kGnomeFiles).starts_with("cut\n"));
    CHECK(f.clipboard.data(views::kKdeCut) == "1");

    // rummage's own: Ctrl+V moves them once, then the clipboard is emptied.
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.clipboard.cleared == 1);

    // Another program copies: what rummage took is forgotten, the cut marks go.
    f.show(f.root / "photos", "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK_FALSE(f.browser.cut_marks().names.empty());
    f.clipboard.copy_from_elsewhere({{"text/plain", "hello"}});
    f.browser.clipboard_lost();
    CHECK(f.browser.cut_marks().names.empty());
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.name_editor().active());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "Pasted.txt"});
    CHECK(rummage::core::read_text_file(f.root / "album/Pasted.txt").text == "hello");
}

TEST_CASE("clipboard: Ctrl+Shift+C copies the shown folder's path as text, whatever is selected") {
    Fixture f;
    const fs::path photos = f.root / "photos";
    f.show(photos, "P1.ORF");
    f.browser.details().folder()->select_all();

    // Files taken before: the text replaces them, Ctrl+V has nothing of Rummage's to paste.
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_C, {.ctrl = true, .shift = true})));
    CHECK(f.clipboard.ours);
    CHECK(f.clipboard.data(views::kText) == photos.string());
    CHECK(f.clipboard.data("UTF8_STRING") == photos.string());
    CHECK(f.clipboard.data("text/plain") == photos.string());
    CHECK(f.clipboard.data(views::kUriList).empty());
    CHECK(f.clipboard.data(views::kGnomeFiles).empty());
    CHECK(f.browser.status_bar().message() == "Copied " + photos.string());
    CHECK(f.browser.details().folder()->selected_count() == 3);  // the selection stays

    // An empty folder too, and the Caps Lock keysym.
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true, .shift = true})));
    CHECK(f.clipboard.data(views::kText) == (f.root / "album").string());
}

TEST_CASE("clipboard: Ctrl+V pastes another program's files: copied, or moved when cut") {
    Fixture f;
    const fs::path p2 = f.root / "photos" / "P2.ORF";
    const fs::path note = f.root / "other" / "note.txt";

    // A URI list from two folders: copied, each from its own.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kUriList}, views::uri_list(Paths{p2, note})}});
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P2.ORF", "note.txt"});
    CHECK(fs::exists(p2));
    CHECK(f.clipboard.cleared == 0);

    // GNOME's "cut": moved, sidecar along, and the clipboard emptied.
    const fs::path p1 = f.root / "photos" / "P1.ORF";
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{p1})},
         {std::string{views::kUriList}, views::uri_list(Paths{p1})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "note.txt"});
    CHECK(listing(f.root / "photos") == Names{"P2.ORF"});
    CHECK(f.clipboard.cleared == 1);

    // KDE's "cut": a URI list and a type of its own.
    f.clipboard.copy_from_elsewhere({{std::string{views::kUriList}, views::uri_list(Paths{note})},
                                     {std::string{views::kKdeCut}, "1"}});
    f.show(f.root / "photos", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "photos") == Names{"P2.ORF", "note.txt"});
    CHECK(listing(f.root / "other").empty());

    // Not files here.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kUriList}, "https://example.com/a.jpg\r\n"}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Only files on this computer can be pasted");
}

TEST_CASE("clipboard: another program's cut is emptied only once something moved") {
    Fixture f;
    const fs::path note = f.root / "other" / "note.txt";
    f.show(f.root / "album", "");

    // Nothing could move (the file is gone): the cut stays on the clipboard.
    const fs::path gone = f.root / "other" / "gone.txt";
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{gone})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.clipboard.cleared == 0);
    CHECK_FALSE(f.clipboard.formats.empty());

    // Something else copied while the move ran: that is not emptied.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{note})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    f.clipboard.copy_from_elsewhere({{"text/plain", "newer"}});
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"note.txt"});
    CHECK(f.clipboard.cleared == 0);
    CHECK(f.clipboard.data("text/plain") == "newer");
}

TEST_CASE("clipboard: Ctrl+V in a name's field pastes the text's first line") {
    Fixture f;
    f.show(f.root / "photos", "P2.ORF");
    f.clipboard.copy_from_elsewhere({{"UTF8_STRING", "Lake\tComo\nsecond line"}});
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    // F2 selects the name without its extension: the pasted text takes its place.
    CHECK(f.browser.name_editor().field().text() == "LakeComo.ORF");
    // Ctrl+Z gives the name back as it was.
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(f.browser.name_editor().field().text() == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_Escape)));
}

TEST_CASE("clipboard: text paste creates a unique file and selects its stem for rename and undo") {
    Fixture f;
    f.show(f.root / "album", "");
    f.browser.set_name_filter("does-not-match");
    const std::string text = "Hello, café!\nSecond line\n";
    f.clipboard.copy_from_elsewhere(
        {{"text/plain;charset=utf-8", text}, {"text/html", "<b>Hello</b>"}});
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.name_filter().empty());
    CHECK(rummage::core::read_text_file(f.root / "album/Pasted.txt").text == text);
    REQUIRE(f.browser.name_editor().active());
    f.browser.name_editor().paste("Notes");
    CHECK(f.browser.name_editor().field().text() == "Notes.txt");
    REQUIRE(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK(rummage::core::read_text_file(f.root / "album/Notes.txt").text == text);
    REQUIRE(f.press(key(XKB_KEY_z, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(fs::exists(f.root / "album/Pasted.txt"));
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(rummage::core::read_text_file(f.root / "album/Pasted 2.txt").text == text);
    f.browser.name_editor().cancel();
    REQUIRE(f.press(key(XKB_KEY_z, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.root / "album/Pasted 2.txt"));
    CHECK(fs::exists(f.root / "album/Pasted.txt"));
    REQUIRE(f.press(key(XKB_KEY_z, {.ctrl = true, .shift = true})));
    REQUIRE(f.settle());
    CHECK(rummage::core::read_text_file(f.root / "album/Pasted 2.txt").text == text);
}

TEST_CASE("clipboard: images preserve exact binary bytes and take precedence over text") {
    for (const auto& [mime, extension] :
         {std::pair{"image/png", ".png"}, {"image/jpeg", ".jpg"}, {"image/webp", ".webp"}}) {
        CAPTURE(mime);
        Fixture f;
        f.show(f.root / "album", "");
        // Clipboard bytes are opaque; decoding remains the isolated previewer's job.
        const std::string bytes{"image\0payload\xff", 14};
        f.clipboard.copy_from_elsewhere({{"text/plain", "image caption"}, {mime, bytes}});
        REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
        REQUIRE(f.settle());
        CHECK(listing(f.root / "album") == Names{std::string{"Pasted"} + extension});
        CHECK(rummage::core::read_text_file(f.root / "album" / (std::string{"Pasted"} + extension))
                  .text == bytes);
        REQUIRE(f.browser.name_editor().active());
        f.browser.name_editor().paste("Screenshot");
        CHECK(f.browser.name_editor().field().text() == std::string{"Screenshot"} + extension);
    }
}

TEST_CASE("clipboard: changing folders during a read does not steal focus for rename") {
    Fixture f;
    f.show(f.root / "album", "");
    f.clipboard.copy_from_elsewhere({{"text/plain", "saved in album"}});
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    f.browser.open(f.root / "other");
    REQUIRE(f.settle());
    CHECK(rummage::core::read_text_file(f.root / "album/Pasted.txt").text == "saved in album");
    CHECK_FALSE(f.browser.name_editor().active());
    CHECK_FALSE(fs::exists(f.root / "other/Pasted.txt"));
}

TEST_CASE("clipboard: unreadable and unsupported offers do not create files") {
    Fixture f;
    f.show(f.root / "album", "");
    auto port = f.clipboard.port();
    port.read = [&](std::string, SystemClipboard::Done done) {
        f.loop.post([done] { done(std::nullopt); });
    };
    f.browser.set_system_clipboard(std::move(port));
    f.clipboard.copy_from_elsewhere({{"image/png", "unavailable"}});
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "The clipboard could not be read");
    CHECK(listing(f.root / "album").empty());
    CHECK_FALSE(f.browser.name_editor().active());
    f.clipboard.copy_from_elsewhere({{"application/octet-stream", "binary"}});
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album").empty());
}

TEST_CASE("clipboard: undo preserves a pasted file edited after creation") {
    Fixture f;
    f.show(f.root / "album", "");
    f.clipboard.copy_from_elsewhere({{"text/plain", "original"}});
    REQUIRE(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    f.browser.name_editor().cancel();
    const auto file = f.root / "album/Pasted.txt";
    const auto modified = fs::last_write_time(file);
    std::ofstream{file} << "modified";  // same byte count
    fs::last_write_time(file, modified + 2s);
    REQUIRE(f.press(key(XKB_KEY_z, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(rummage::core::read_text_file(file).text == "modified");
    CHECK(f.browser.status_bar().message().find("has changed since it was made") !=
          std::string::npos);
}

TEST_CASE("clipboard: Ctrl+C and Ctrl+X in a name's field copy and cut its selection as text") {
    Fixture f;
    f.show(f.root / "photos", "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    REQUIRE_FALSE(f.browser.cut_marks().names.empty());
    CHECK(f.press(key(XKB_KEY_F2)));
    const rummage::ui::TextField& field = f.browser.name_editor().field();

    // F2 selects the name without its extension: Ctrl+C offers it as text, not the file.
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.clipboard.ours);
    CHECK(f.clipboard.data(views::kText) == "P2");
    CHECK(f.clipboard.data("UTF8_STRING") == "P2");
    CHECK(f.clipboard.data("text/plain") == "P2");
    CHECK(f.clipboard.data(views::kGnomeFiles).empty());
    CHECK(f.browser.cut_marks().names.empty());
    CHECK(f.browser.status_bar().message() == "Copied 2 characters");
    CHECK(field.text() == "P2.ORF");

    // Ctrl+X takes it out; one Ctrl+Z brings it back.
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(field.text() == ".ORF");
    CHECK(f.clipboard.data(views::kText) == "P2");
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.text() == "P2.ORF");

    // Nothing selected: the keys are the field's all the same, and nothing changes.
    CHECK(f.press(key(XKB_KEY_End)));
    f.clipboard.copy_from_elsewhere({{"text/plain", "kept"}});
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK_FALSE(f.clipboard.ours);
    CHECK(f.clipboard.data("text/plain") == "kept");
    CHECK(field.text() == "P2.ORF");

    // What was cut pastes back.
    CHECK(f.press(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(field.text().empty());
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(field.text() == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_Escape)));
}

TEST_CASE("clipboard: while a rename is on its way Ctrl+X leaves the name, Ctrl+C still copies") {
    Fixture f;
    f.show(f.root / "photos", "P2.ORF");
    CHECK(f.press(key(XKB_KEY_F2)));
    f.browser.name_editor().paste("P3");
    const rummage::ui::TextField& field = f.browser.name_editor().field();
    REQUIRE(field.text() == "P3.ORF");
    CHECK(f.press(key(XKB_KEY_Home, {.shift = true})));
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.browser.name_editor().busy());

    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(field.text() == "P3.ORF");
    CHECK_FALSE(f.clipboard.ours);
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kText) == "P3");
    REQUIRE(f.settle());
    CHECK(listing(f.root / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P3.ORF"});
}

TEST_CASE("clipboard: location and name dialogs copy and cut text without taking files") {
    Fixture f;
    f.show(f.root / "photos", "P2.ORF");
    f.browser.location_dialog().open("/tmp/example",
                                     [](const auto&, bool) { return std::string{}; });
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kText) == "/tmp/example");
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.browser.location_dialog().text().empty());
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(f.browser.location_dialog().text() == "/tmp/example");
    CHECK(f.clipboard.data(views::kGnomeFiles).empty());
    f.browser.location_dialog().close();
    f.browser.confirm_dialog().open_name("Name", "", "archive.zip",
                                         [](const auto&) { return std::string{}; });
    CHECK(f.press(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kText) == "archive.zip");
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kText) == "archive.zip");
    CHECK(f.browser.cut_marks().names.empty());
}
