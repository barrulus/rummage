#include "rummage/views/context_menu.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::ContextMenu;
using rummage::views::Folder;
using rummage::views::MenuItem;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

TEST_CASE("i18n: menu rendering preserves literal names in every language") {
    struct RestoreEnglish {
        ~RestoreEnglish() { rummage::i18n::set_language("en"); }
    } restore;

    rummage::ui::Offscreen surface{400, 200};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    ContextMenu menu{[] {}};
    MenuItem literal;
    literal.id = "template:Home";
    literal.label = "Home";
    menu.open(20, 20, {literal}, true);
    const auto pixels = [&](std::string_view language) {
        rummage::i18n::set_language(language);
        renderer.begin_frame(400, 200, 1, {});
        menu.draw(renderer, {0, 0, 400, 200}, rummage::theme::Palette{});
        renderer.end_frame();
        std::vector<unsigned char> result(400 * 200 * 4);
        glReadPixels(0, 0, 400, 200, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        return result;
    };
    const auto english = pixels("en");
    const auto german = pixels("de");
    REQUIRE(rummage::i18n::tr("Home") != "Home");
    CHECK(english == german);
}

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

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

MenuItem item(std::string id, bool enabled = true, std::function<void()> action = nullptr) {
    MenuItem out;
    out.label = id;
    out.id = std::move(id);
    out.enabled = enabled;
    out.action = std::move(action);
    return out;
}

// The ids of the menu's items, and which can be chosen ("-paste": shown, not usable).
Names ids(const std::vector<MenuItem>& items, bool separators = false) {
    Names out;
    for (const MenuItem& entry : items) {
        if (separators && entry.separator)
            out.emplace_back("|");
        out.push_back(entry.enabled ? entry.id : "-" + entry.id);
    }
    return out;
}

// photos/{a.txt, b.txt, P1.ORF, P1.ORF.xmp, sub/}, shown in the details.
struct Fixture {
    TempDir dir;
    fs::path photos = dir.path / "photos";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    // What "Open With" was asked and did.
    std::vector<Browser::AppChoice> apps{
        {.id = "viewer.desktop", .name = "Viewer", .is_default = true},
        {.id = "editor.desktop", .name = "Editor"}};
    std::string asked_type;
    std::string opened_with;
    Names opened;
    std::vector<fs::path> terminals;  // where F4 and "Open in Terminal" opened one

    Fixture() {
        fs::create_directories(photos / "sub");
        for (const char* name : {"a.txt", "b.txt", "P1.ORF", "P1.ORF.xmp"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(dir.path / "Trash");
        browser.set_templates_dir(dir.path / "templates");
        browser.set_open_with_handlers(
            [this](const fs::path& /*directory*/, const rummage::fs::Entry& entry,
                   const std::function<void(std::vector<Browser::AppChoice>)>& done) {
                asked_type = entry.name;
                done(apps);
            },
            [this](const fs::path& /*directory*/, const std::vector<rummage::fs::Entry>& files,
                   std::string app_id) {
                opened_with = std::move(app_id);
                opened.clear();
                for (const rummage::fs::Entry& file : files) {
                    opened.push_back(file.name);
                }
            });
        browser.set_terminal_handler(
            [this](const fs::path& directory) { terminals.push_back(directory); });
        browser.open(photos);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> folder() { return browser.details().folder(); }

    ContextMenu& menu() { return browser.context_menu(); }

    void select(const Names& names) {
        folder()->set_cursor(std::nullopt);
        for (const std::string& name : names) {
            const auto& entries = folder()->entries();
            const auto it = std::ranges::find(entries, name, &rummage::fs::Entry::name);
            REQUIRE(it != entries.end());
            folder()->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                                 rummage::views::SelectMode::toggle);
        }
    }

    // Down until the item called id is highlighted, then Enter.
    void choose(const std::string& id) {
        for (std::size_t i = 0; i < 20; ++i) {
            const std::optional<std::size_t> at = menu().highlighted();
            if (at && menu().items()[*at].id == id) {
                CHECK(press(key(XKB_KEY_Return)));
                return;
            }
            press(key(XKB_KEY_Down));
        }
        FAIL("no item " << id);
    }
};

}  // namespace

TEST_CASE("context menu: the keyboard moves over what can be chosen, in and out of a submenu") {
    ContextMenu menu{[] {}};
    int ran = 0;
    MenuItem with = item("with");
    with.submenu = true;
    with.children = {item("first", true, [&ran] { ran = 1; }), item("off", false),
                     item("last", true, [&ran] { ran = 3; })};
    MenuItem later = item("later");
    later.submenu = true;
    later.empty_text = "Looking…";
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; }), item("paste", false), with, later},
              true);
    REQUIRE(menu.is_open());
    CHECK(menu.highlighted() == 0);  // from the keyboard: the first, at once
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.highlighted() == 2);  // over Paste, which cannot be chosen
    CHECK(menu.key(key(XKB_KEY_Up)));
    CHECK(menu.highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Up)));
    CHECK(menu.highlighted() == 3);  // wraps
    CHECK(menu.key(key(XKB_KEY_Home)));
    CHECK(menu.highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_q)));  // every key is the menu's: nothing happens
    CHECK(menu.is_open());

    // Right opens the submenu, its first item highlighted; Down skips the disabled one.
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.submenu_open());
    CHECK(menu.sub_highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.sub_highlighted() == 2);
    CHECK(menu.key(key(XKB_KEY_Left)));  // back out
    CHECK_FALSE(menu.submenu_open());
    CHECK(menu.highlighted() == 2);
    CHECK(menu.key(key(XKB_KEY_Return)));  // Enter on a submenu opens it
    CHECK(menu.submenu_open());
    CHECK(menu.key(key(XKB_KEY_Escape)));  // Escape: out of the submenu first
    CHECK(menu.is_open());
    CHECK_FALSE(menu.submenu_open());

    // A submenu filled later: the keyboard waiting in it lands on its first item.
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.submenu_open());
    CHECK_FALSE(menu.sub_highlighted().has_value());
    menu.set_submenu("later", {item("found", true, [&ran] { ran = 4; })});
    CHECK(menu.sub_highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 4);
    CHECK_FALSE(menu.is_open());  // closed before the action ran

    // Escape closes; Enter runs an item.
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, true);
    CHECK(menu.key(key(XKB_KEY_Escape)));
    CHECK_FALSE(menu.is_open());
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, true);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 10);

    // A click before it was ever drawn lands outside: it closes, nothing runs.
    ran = 0;
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, false);
    CHECK_FALSE(menu.highlighted().has_value());  // by the pointer: nothing highlighted yet
    rummage::ui::PointerEvent click;
    click.kind = rummage::ui::PointerEvent::Kind::press;
    click.button = rummage::ui::PointerButton::left;
    CHECK(menu.pointer(click));
    CHECK_FALSE(menu.is_open());
    CHECK(ran == 0);
}

TEST_CASE("context menu: the items for one entry, several, empty space; paste once taken") {
    Fixture f;
    // One file: everything, Rename too; nothing taken yet, so no Paste.
    f.select({"a.txt"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.menu().is_open());
    CHECK(ids(f.menu().items(), true) == Names{"open",
                                               "open_with",
                                               "|",
                                               "cut",
                                               "copy",
                                               "-paste",
                                               "rename",
                                               "create_link",
                                               "create_relative_link",
                                               "create_hard_link",
                                               "move_into_folder",
                                               "|",
                                               "action:copy_name",
                                               "action:copy_location",
                                               "|",
                                               "compress_zip",
                                               "compress_tar",
                                               "|",
                                               "trash",
                                               "erase",
                                               "|",
                                               "sort",
                                               "panes",
                                               "|",
                                               "properties"});
    CHECK(f.menu().items()[3].keys == "Ctrl+C");  // from the shortcuts table
    const auto trash = std::ranges::find(f.menu().items(), "trash", &MenuItem::id);
    REQUIRE(trash != f.menu().items().end());
    CHECK(trash->keys == "Delete");
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(f.menu().is_open());

    // Several: batch Rename. A folder: Open With and a terminal too.
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    CHECK(ids(f.menu().items(), true) == Names{"open",
                                               "open_with",
                                               "|",
                                               "cut",
                                               "copy",
                                               "-paste",
                                               "rename",
                                               "create_link",
                                               "create_relative_link",
                                               "create_hard_link",
                                               "move_into_folder",
                                               "|",
                                               "action:copy_name",
                                               "action:copy_location",
                                               "|",
                                               "compress_zip",
                                               "compress_tar",
                                               "|",
                                               "trash",
                                               "erase",
                                               "|",
                                               "sort",
                                               "panes",
                                               "|",
                                               "-properties"});
    f.menu().close();
    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_F10, {.shift = true})));
    CHECK(ids(f.menu().items(), true) == Names{"open",
                                               "open_with",
                                               "open_tab",
                                               "open_terminal",
                                               "|",
                                               "cut",
                                               "copy",
                                               "-paste",
                                               "rename",
                                               "create_link",
                                               "create_relative_link",
                                               "create_hard_link",
                                               "move_into_folder",
                                               "|",
                                               "action:copy_name",
                                               "action:copy_location",
                                               "|",
                                               "compress_zip",
                                               "compress_tar",
                                               "|",
                                               "add_bookmark",
                                               "|",
                                               "trash",
                                               "erase",
                                               "|",
                                               "sort",
                                               "panes",
                                               "|",
                                               "properties",
                                               "folder_insights"});

    // Copy from the menu; then Paste can be chosen, here and on empty space.
    f.choose("copy");
    CHECK_FALSE(f.menu().is_open());
    CHECK(f.browser.status_bar().message() == "Ctrl+V copies “sub” where you want them");
    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(ids(f.menu().items(), true) == Names{"new_folder", "new_file", "|", "paste",
                                               "paste_links", "action:copy_location", "|",
                                               "open_terminal", "add_bookmark", "|", "sort",
                                               "panes", "|", "properties", "folder_insights"});
    REQUIRE(f.settle());  // the templates looked for: none
    CHECK(ids(f.menu().items()[1].children) == Names{"empty_file"});
}

TEST_CASE("context menu: Sort By sorts every view, and the folders they open later") {
    Fixture f;
    // Sizes 1, 30, 200 and 5000 bytes; "sub" stays first, folders before files.
    std::ofstream{f.photos / "a.txt"} << std::string(5000, 'a');
    std::ofstream{f.photos / "b.txt"} << std::string(1, 'b');
    std::ofstream{f.photos / "P1.ORF"} << std::string(200, 'p');
    std::ofstream{f.photos / "P1.ORF.xmp"} << std::string(30, 'x');
    fs::create_directories(f.photos / "sub" / "deeper");
    std::ofstream{f.photos / "sub" / "big"} << std::string(900, 'g');
    std::ofstream{f.photos / "sub" / "small"} << "s";
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(run_until(f.loop, [&f] {
        const auto& entries = f.browser.grid().folder()->entries();
        const auto a = std::ranges::find(entries, "a.txt", &rummage::fs::Entry::name);
        return a != entries.end() && a->size == 5000;
    }));
    const auto names = [](const Folder& folder) {
        Names out;
        for (const rummage::fs::Entry& entry : folder.entries()) {
            out.push_back(entry.name);
        }
        return out;
    };
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "b.txt", "P1.ORF", "P1.ORF.xmp"});

    // The menu: a submenu with a dot on the order in use, on empty space and on an entry.
    f.select({});
    f.browser.open_menu(100, 100, false);
    const MenuItem& sort = *std::ranges::find(f.menu().items(), "sort", &MenuItem::id);
    REQUIRE(sort.id == "sort");
    CHECK(sort.submenu);
    CHECK(ids(sort.children) == Names{"sort:name", "sort:size", "sort:modified", "sort:type",
                                      "sort:created", "sort:accessed", "sort:owner", "sort:group",
                                      "sort:permissions", "sort:location", "sort:link_target",
                                      "sort:ascending", "sort:descending", "reset_folder_view"});
    const auto checked = [](const MenuItem& submenu) {
        Names out;
        for (const MenuItem& item : submenu.children) {
            CHECK(item.radio == (item.id != "reset_folder_view"));
            if (item.checked) {
                out.push_back(item.id);
            }
        }
        return out;
    };
    CHECK(checked(sort) == Names{"sort:name", "sort:ascending"});

    // Size, then Descending: the grid's folder follows, largest first.
    f.choose("sort");
    CHECK(f.press(key(XKB_KEY_Right)));  // into the submenu, on Name
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Return)));  // Size
    CHECK_FALSE(f.menu().is_open());
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "b.txt", "P1.ORF.xmp", "P1.ORF", "a.txt"});
    f.select({"a.txt"});
    f.browser.open_menu(100, 100, false);
    const auto sorted = std::ranges::find(f.menu().items(), "sort", &MenuItem::id);
    REQUIRE(sorted != f.menu().items().end());
    CHECK(checked(*sorted) == Names{"sort:size", "sort:ascending"});
    const auto descending = std::ranges::find(sorted->children, "sort:descending", &MenuItem::id);
    REQUIRE(descending != sorted->children.end());
    descending->action();
    f.menu().close();
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "P1.ORF", "P1.ORF.xmp", "b.txt"});
    CHECK(f.browser.state().sort_key == rummage::fs::SortKey::size);
    CHECK(f.browser.state().sort_descending);

    // A folder opened afterwards, in the columns, is sorted so too; the details show it.
    f.browser.open(f.photos / "sub");
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    REQUIRE(run_until(
        f.loop, [&f] { return f.browser.columns().focused_folder()->entries().size() == 3; }));
    CHECK(names(*f.browser.columns().focused_folder()) == Names{"deeper", "big", "small"});
    CHECK(f.browser.details().order().key == rummage::fs::SortKey::size);

    // A details header sorts this folder; returning restores the parent's own order.
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().sort_by(rummage::views::DetailsView::Field::name);  // name, ascending
    f.browser.set_mode(Browser::Mode::grid);
    CHECK(names(*f.browser.grid().folder()) == Names{"deeper", "big", "small"});
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::name);
    f.browser.open(f.photos);
    REQUIRE(f.settle());
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "P1.ORF", "P1.ORF.xmp", "b.txt"});
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    CHECK(f.browser.sort_order().descending);
}

TEST_CASE("context menu: Open With lists the applications and opens the selection with one") {
    Fixture f;
    f.select({"sub", "b.txt", "a.txt"});  // the cursor on a.txt
    REQUIRE(f.folder()->cursor_entry() != nullptr);
    const std::string cursor = f.folder()->cursor_entry()->name;
    REQUIRE(cursor == "a.txt");
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.asked_type == cursor);  // the applications of the cursor's file
    const MenuItem& with = f.menu().items()[1];
    REQUIRE(with.id == "open_with");
    REQUIRE(with.children.size() == 2);
    CHECK(with.children[0].label == "Viewer (default)");
    CHECK(with.children[1].label == "Editor");
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(f.opened_with == "editor.desktop");
    std::ranges::sort(f.opened);
    CHECK(f.opened == Names{"a.txt", "b.txt"});  // the files; folders are not opened

    // No applications: a dimmed line says so.
    f.apps.clear();
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.menu().items()[1].children.size() == 1);
    CHECK(f.menu().items()[1].children[0].label == "No applications");
    CHECK_FALSE(f.menu().items()[1].children[0].enabled);
}

TEST_CASE("context menu: F4 and Open in Terminal: the cursor's folder, else the one shown") {
    Fixture f;
    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_F4)));
    f.select({"a.txt"});
    CHECK(f.press(key(XKB_KEY_F4)));
    CHECK(f.terminals == std::vector<fs::path>{f.photos / "sub", f.photos});

    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.menu().items()[3].keys == "F4");
    f.choose("open_terminal");
    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("open_terminal");
    CHECK(f.terminals ==
          std::vector<fs::path>{f.photos / "sub", f.photos, f.photos / "sub", f.photos});

    // A folder opens with a chosen application; the files selected with it are left out.
    f.select({"a.txt", "sub"});  // the cursor on sub
    REQUIRE(f.folder()->cursor_entry() != nullptr);
    REQUIRE(f.folder()->cursor_entry()->name == "sub");
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.asked_type == "sub");
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(f.opened_with == "viewer.desktop");
    CHECK(f.opened == Names{"sub"});
}

TEST_CASE("context menu: Move to Trash and Rename from the menu do what their keys do") {
    Fixture f;
    f.select({"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("trash");
    REQUIRE(f.settle());
    CHECK(fs::exists(f.dir.path / "Trash" / "files" / "P1.ORF.xmp"));
    CHECK_FALSE(fs::exists(f.photos / "P1.ORF"));

    REQUIRE(run_until(f.loop, [&] { return f.folder()->cursor_entry() != nullptr; }));
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("rename");
    CHECK(f.browser.name_editor().active());
}

TEST_CASE("context menu: New File, empty or from a template, never over an existing file") {
    Fixture f;
    const fs::path templates = f.dir.path / "templates";
    fs::create_directories(templates / "folder");
    std::ofstream{templates / "Letter.txt"} << "Dear,";
    chmod((templates / "Letter.txt").c_str(), 0640);
    std::ofstream{templates / ".hidden"} << "x";
    std::ofstream{templates / "Notes.md~"} << "x";
    CHECK(rummage::fs::templates_in(templates) == Names{"Letter.txt"});

    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.settle());
    CHECK(ids(f.menu().items()[1].children) == Names{"empty_file", "template:Letter.txt"});
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));  // Empty File
    REQUIRE(f.settle());
    CHECK(fs::is_regular_file(f.photos / "New File"));
    CHECK(fs::file_size(f.photos / "New File") == 0);
    REQUIRE(run_until(f.loop, [&] { return f.browser.name_editor().active(); }));
    f.browser.name_editor().cancel();

    for (int i = 0; i < 2; ++i) {
        f.select({});
        CHECK(f.press(key(XKB_KEY_Menu)));
        REQUIRE(f.settle());
        CHECK(f.press(key(XKB_KEY_Down)));
        CHECK(f.press(key(XKB_KEY_Right)));
        CHECK(f.press(key(XKB_KEY_Down)));
        CHECK(f.press(key(XKB_KEY_Return)));  // Letter.txt
        REQUIRE(f.settle());
        f.browser.name_editor().cancel();
    }
    CHECK(read_file(f.photos / "Letter.txt") == "Dear,");
    CHECK(read_file(f.photos / "Letter 2.txt") == "Dear,");
    CHECK((fs::status(f.photos / "Letter 2.txt").permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
}

TEST_CASE("new file: numbered names, a template's contents, nothing left on failure") {
    const TempDir dir;
    std::ofstream{dir.path / "New File"} << "old";
    const rummage::fs::MadeFile empty = rummage::fs::make_file(dir.path, "New File");
    CHECK(empty.name == "New File 2");
    CHECK(read_file(dir.path / "New File") == "old");
    CHECK(rummage::fs::make_file(dir.path, "a.tar.gz").name == "a.tar.gz");
    CHECK(rummage::fs::make_file(dir.path, "a.tar.gz").name == "a.tar 2.gz");
    const rummage::fs::MadeFile missing =
        rummage::fs::make_file(dir.path, "x.txt", dir.path / "no template");
    CHECK(missing.error == std::errc::no_such_file_or_directory);
    CHECK_FALSE(fs::exists(dir.path / "x.txt"));
}

TEST_CASE("new file: the templates folder, as the user dirs say") {
    const TempDir dir;
    rummage::fs::PlaceSources sources{.home = dir.path / "home",
                                      .config_home = dir.path / "config"};
    fs::create_directories(sources.home / "Templates");
    fs::create_directories(sources.home / "Modelli");
    fs::create_directories(sources.config_home);
    CHECK(rummage::fs::templates_dir(sources) == sources.home / "Templates");  // no user-dirs
    std::ofstream{sources.config_home / "user-dirs.dirs"}
        << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n";
    CHECK(rummage::fs::templates_dir(sources).empty());  // the file says nothing about it
    std::ofstream{sources.config_home / "user-dirs.dirs"}
        << "XDG_TEMPLATES_DIR=\"$HOME/Modelli\"\n";
    CHECK(rummage::fs::templates_dir(sources) == sources.home / "Modelli");
    std::ofstream{sources.config_home / "user-dirs.dirs"} << "XDG_TEMPLATES_DIR=\"$HOME/Nope\"\n";
    CHECK(rummage::fs::templates_dir(sources).empty());  // not there
}

TEST_CASE("actions: copy location shares menu and palette and captures its target") {
    Fixture f;
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    f.choose("action:copy_location");
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().second ==
          (f.photos / "a.txt").string() + '\n' + (f.photos / "b.txt").string());
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    f.select({"sub"});
    auto items = f.browser.palette_items();
    const auto item =
        std::ranges::find(items, "action:copy_location", &rummage::views::PaletteItem::id);
    REQUIRE(item != items.end());
    f.select({"a.txt"});
    item->action();
    CHECK(offered.front().second == (f.photos / "sub").string());
    f.select({});
    f.browser.open_menu(100, 100, false);
    f.choose("action:copy_location");
    CHECK(offered.front().second == f.photos.string());
}

TEST_CASE("actions: copy name shares menu and palette and preserves exact basenames") {
    Fixture f;
    const std::string tricky = "Photo ü ;'$(x).tar.gz";
    std::ofstream{f.photos / tricky} << "photo";
    f.browser.open(f.photos / "sub");
    REQUIRE(f.settle());
    f.browser.open(f.photos);
    REQUIRE(f.settle());
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.select({tricky});
    f.browser.open_menu(100, 100, false);
    const auto menu_item = std::ranges::find(f.menu().items(), "action:copy_name", &MenuItem::id);
    REQUIRE(menu_item != f.menu().items().end());
    CHECK(menu_item->keys == "Ctrl+Alt+C");
    f.choose("action:copy_name");
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    CHECK(offered.front().second == tricky);
    f.select({"a.txt", "b.txt"});
    auto items = f.browser.palette_items();
    const auto name =
        std::ranges::find(items, "action:copy_name", &rummage::views::PaletteItem::id);
    REQUIRE(name != items.end());
    f.select({"sub"});
    name->action();
    CHECK(offered.front().second == "a.txt\nb.txt");
    items = f.browser.palette_items();
    const auto folder_name =
        std::ranges::find(items, "action:copy_name", &rummage::views::PaletteItem::id);
    REQUIRE(folder_name != items.end());
    folder_name->action();
    CHECK(offered.front().second == "sub");
    f.select({});
    items = f.browser.palette_items();
    CHECK(std::ranges::find(items, "action:copy_name", &rummage::views::PaletteItem::id) ==
          items.end());
}

TEST_CASE("actions: copy name in Trash uses the displayed original name") {
    struct RestoreSources {
        rummage::fs::TrashSources before{rummage::fs::trash_sources()};

        ~RestoreSources() { rummage::fs::set_trash_sources(before); }
    } restore_sources;

    Fixture f;
    const auto trash = f.dir.path / "Trash";
    fs::create_directories(trash / "files");
    fs::create_directories(trash / "info");
    std::ofstream{trash / "files" / "stored-id"} << "raw";
    std::ofstream{trash / "info" / "stored-id.trashinfo"}
        << rummage::fs::trash_info(f.photos / "Photo Name.ORF", 0);
    rummage::fs::set_trash_sources(
        {.home = trash, .tops = std::vector<fs::path>{}, .uid = geteuid()});
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.browser.open(rummage::fs::trash_path());
    REQUIRE(f.settle());
    f.select({"Photo Name.ORF"});
    auto items = f.browser.palette_items();
    const auto name =
        std::ranges::find(items, "action:copy_name", &rummage::views::PaletteItem::id);
    REQUIRE(name != items.end());
    name->action();
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().second == "Photo Name.ORF");
    CHECK(std::ranges::find(items, "action:copy_location", &rummage::views::PaletteItem::id) ==
          items.end());
    f.browser.open_menu(100, 100, false);
    f.choose("action:copy_name");
    CHECK(offered.front().second == "Photo Name.ORF");
}

TEST_CASE("actions: custom handoffs share applicability and capture selection across navigation") {
    Fixture f;
    using rummage::core::Action;
    std::vector<Action> actions{{.id = "inspect",
                                 .label = "Inspect",
                                 .argv = {"inspector", "{files}"},
                                 .mime = {"text/plain"},
                                 .selection = Action::Selection::many}};
    SUBCASE("Actions submenu") {}
    SUBCASE("parent menu") { actions.front().placement = Action::Placement::menu; }
    const bool parent = actions.front().placement == Action::Placement::menu;
    f.browser.set_actions(actions);
    std::vector<std::string> launched;
    fs::path directory;
    bool available = true;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
            directory = context.directory;
        },
        [&](const auto&, const auto&) { return available; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    const auto group =
        std::ranges::find(f.menu().items(), parent ? "action:inspect" : "actions", &MenuItem::id);
    REQUIRE(group != f.menu().items().end());
    if (parent) {
        CHECK_FALSE(group->submenu);
        CHECK(std::ranges::find(f.menu().items(), "actions", &MenuItem::id) ==
              f.menu().items().end());
    } else {
        REQUIRE(group->children.size() == 1);
        CHECK(group->children.front().id == "action:inspect");
    }
    const auto run = parent ? group->action : group->children.front().action;
    f.menu().close();
    f.browser.open(f.photos / "sub");
    REQUIRE(f.settle());
    run();
    CHECK(directory == f.photos);
    CHECK(launched == std::vector<std::string>{"inspector", (f.photos / "a.txt").string(),
                                               (f.photos / "b.txt").string()});
    f.browser.open(f.photos);
    REQUIRE(f.settle());
    f.select({"a.txt", "sub"});
    const auto mixed = f.browser.palette_items();
    CHECK(std::ranges::find(mixed, "action:inspect", &rummage::views::PaletteItem::id) ==
          mixed.end());
    f.select({"a.txt"});
    const auto supported = f.browser.palette_items();
    REQUIRE(std::ranges::find(supported, "action:inspect", &rummage::views::PaletteItem::id) !=
            supported.end());
    available = false;
    f.browser.open_menu(100, 100, false);
    const auto missing =
        std::ranges::find(f.menu().items(), parent ? "action:inspect" : "actions", &MenuItem::id);
    REQUIRE(missing != f.menu().items().end());
    const bool enabled = parent ? missing->enabled : missing->children.front().enabled;
    CHECK_FALSE(enabled);
    const auto unavailable = f.browser.palette_items();
    CHECK(std::ranges::find(unavailable, "action:inspect", &rummage::views::PaletteItem::id) ==
          unavailable.end());
}

TEST_CASE("actions: parent menu actions precede the submenu in configuration order") {
    Fixture f;
    using rummage::core::Action;
    const std::vector<Action> actions{{.id = "goblin",
                                       .label = "Goblin",
                                       .argv = {"goblin"},
                                       .mime = {},
                                       .selection = Action::Selection::many},
                                      {.id = "compress",
                                       .label = "Compress…",
                                       .argv = {"archiver", "{files}"},
                                       .mime = {},
                                       .selection = Action::Selection::many,
                                       .placement = Action::Placement::menu},
                                      {.id = "inspect",
                                       .label = "Inspect",
                                       .argv = {"inspector", "{files}"},
                                       .mime = {},
                                       .selection = Action::Selection::many,
                                       .placement = Action::Placement::menu}};
    f.browser.set_actions(actions);
    std::vector<std::string> launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
        },
        [](const auto&, const auto&) { return true; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    const auto& items = f.menu().items();
    const auto group = std::ranges::find(items, "actions", &MenuItem::id);
    REQUIRE(group != items.end());
    REQUIRE(group - items.begin() >= 2);
    CHECK((group - 2)->id == "action:compress");
    CHECK((group - 1)->id == "action:inspect");
    CHECK(ids(group->children) == Names{"action:goblin"});
    f.choose("action:compress");
    CHECK(launched ==
          Names{"archiver", (f.photos / "a.txt").string(), (f.photos / "b.txt").string()});

    f.select({});
    f.browser.open_menu(100, 100, true);
    const auto background = ids(f.menu().items());
    CHECK(std::ranges::find(background, "action:compress") == background.end());
    CHECK(std::ranges::find(background, "actions") == background.end());
}

TEST_CASE("actions: archive presets appear in menu and palette and hand off literal paths") {
    Fixture f;
    const auto archive =
        f.dir.write("two words.zip", std::string{"PK\005\006"} + std::string(18, '\0'));
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.select({"two words.zip"});
    std::vector<std::string> launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = {action.id, context.targets.front().path.string()};
        },
        [](const auto&, const auto&) { return true; });
    f.browser.open_menu(100, 100, false);
    const auto here = std::ranges::find(f.menu().items(), "action:extract_here", &MenuItem::id);
    REQUIRE(here != f.menu().items().end());
    REQUIRE(here->enabled);
    f.choose("action:extract_here");
    CHECK(launched == std::vector<std::string>{"extract_here", archive.string()});
    const auto items = f.browser.palette_items();
    const auto to = std::ranges::find(items, "action:extract_to", &rummage::views::PaletteItem::id);
    REQUIRE(to != items.end());
    to->action();
    CHECK(launched == std::vector<std::string>{"extract_to", archive.string()});
    f.browser.set_action_handoff({}, {});
    const auto picker_items = f.browser.palette_items();
    CHECK(std::ranges::find(picker_items, "action:extract_to", &rummage::views::PaletteItem::id) ==
          picker_items.end());
}

TEST_CASE("context menu: keyboard enters arbitrary nesting and backs out one level") {
    ContextMenu menu{[] {}};
    int ran = 0;
    auto leaf = item("leaf", true, [&] {
        ++ran;
        CHECK_FALSE(menu.is_open());
    });
    auto nested = leaf;
    for (int depth = 0; depth < 8; ++depth) {
        auto group = item("group" + std::to_string(depth));
        group.submenu = true;
        group.children = {item("disabled", false), nested};
        nested = std::move(group);
    }
    menu.open(10, 10, {nested}, true);
    for (std::size_t depth = 1; depth <= 8; ++depth) {
        CHECK(menu.key(key(XKB_KEY_Right)));
        CHECK(menu.open_levels() == depth + 1);
        CHECK(menu.highlighted(depth) == 1);
    }
    CHECK(menu.key(key(XKB_KEY_Escape)));
    CHECK(menu.open_levels() == 8);
    CHECK(menu.key(key(XKB_KEY_Left)));
    CHECK(menu.open_levels() == 7);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 1);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("context menu: asynchronously filled nested groups preserve keyboard navigation") {
    ContextMenu menu{[] {}};
    int ran = 0;
    auto dynamic = item("open_with");
    dynamic.submenu = true;
    dynamic.empty_text = "Loading…";
    auto outer = item("outer");
    outer.submenu = true;
    outer.children = {dynamic};
    menu.open(10, 10, {outer}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.open_levels() == 3);
    CHECK_FALSE(menu.highlighted(2));
    menu.set_submenu("open_with", {item("viewer", true, [&] { ++ran; })});
    CHECK(menu.highlighted(2) == 0);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 1);

    // Updating a parent while one of its descendants is open drops stale descendant indices.
    dynamic.children = {outer};
    menu.open(10, 10, {dynamic}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    menu.set_submenu("open_with", {item("replacement", false)});
    CHECK(menu.open_levels() == 2);
    CHECK_FALSE(menu.highlighted(1));
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(menu.is_open());

    auto templates = item("new_file");
    templates.submenu = true;
    menu.open(10, 10, {item("empty_file"), templates}, true);
    auto letter = item("template:Letter");
    letter.separator = true;
    menu.set_submenu("new_file", {item("empty_file"), letter});
    REQUIRE(menu.items()[1].children.size() == 1);
    CHECK(menu.items()[1].children[0].id == "template:Letter");
    CHECK_FALSE(menu.items()[1].children[0].separator);
}

TEST_CASE("context menu: layout moves built-ins and actions without duplicates") {
    const auto parsed = rummage::core::parse_config(R"(
[[actions]]
id = "script"
label = "Script"
argv = ["tool", "{files}"]
selection = "many"
[[actions]]
id = "external"
label = "External tool"
argv = ["external", "{files}"]
selection = "many"
[context_menu]
items = [
  { separator = true },
  { label = "My Actions", items = [
    { label = "Scripts", items = [{ id = "action:script", label = "Run script" }] },
    { label = "External", items = [{ id = "action:external" }, { id = "open_with" }] },
  ] },
  { id = "copy", label = "Copy these", separator = true },
  { label = "Not applicable", items = [{id = "restore"}] },
  { remaining = true },
  { separator = true },
]
background = [{ label = "Create", items = [{id = "new_file"}, {id = "new_folder"}] }]
sidebar = []
)",
                                                    "menu.toml");
    REQUIRE(parsed.problems.empty());
    Fixture f;
    f.browser.set_actions(parsed.config.actions);
    f.browser.set_context_menu(parsed.config.context_menu);
    Names launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
        },
        [](const auto&, const auto&) { return true; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(10, 10, true);
    const auto& items = f.menu().items();
    REQUIRE(items.size() > 2);
    CHECK(items[0].label == "My Actions");
    CHECK_FALSE(items[0].separator);
    REQUIRE(items[0].children.size() == 2);
    const auto& scripts = items[0].children[0];
    CHECK(scripts.label == "Scripts");
    REQUIRE(scripts.children.size() == 1);
    CHECK(scripts.children.front().label == "Run script");
    const auto& external = items[0].children[1];
    CHECK(external.label == "External");
    REQUIRE(external.children.size() == 2);
    CHECK(external.children[1].id == "open_with");
    CHECK(external.children[1].children.size() == 2);  // async loader found its nested group
    CHECK(items[1].id == "copy");
    CHECK(items[1].keys == "Ctrl+C");
    CHECK(items[1].separator);
    const auto root_ids = ids(items);
    CHECK(std::ranges::count(root_ids, "copy") == 1);
    CHECK(std::ranges::find(root_ids, "actions") == root_ids.end());
    CHECK(std::ranges::find(root_ids, "open_with") == root_ids.end());
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(launched == Names{"tool", (f.photos / "a.txt").string(), (f.photos / "b.txt").string()});
    // Layout hiding/relabeling doesn't remove registry actions from the palette.
    const auto palette = f.browser.palette_items();
    CHECK(std::ranges::find(palette, "action:script", &rummage::views::PaletteItem::id) !=
          palette.end());

    f.select({});
    f.browser.open_menu(10, 10, true);
    REQUIRE(f.menu().items().size() == 1);
    CHECK(f.menu().items()[0].label == "Create");
    REQUIRE(f.menu().items()[0].children.size() == 2);
    CHECK(f.menu().items()[0].children[0].id == "new_file");
    auto hidden = parsed.config.context_menu;
    hidden.background = std::vector<rummage::core::MenuNode>{};
    f.browser.set_context_menu(hidden);
    f.browser.open_menu(10, 10, true);
    CHECK_FALSE(f.menu().is_open());
    CHECK(f.settle());  // hiding a menu must not leave unstarted loaders counted as pending
}

TEST_CASE("context menu: remaining items may be nested and empty groups are omitted") {
    using rummage::core::MenuNode;
    auto group = item("actions");
    group.submenu = true;
    group.children = {item("action:a"), item("action:b")};
    const std::vector<MenuItem> available{item("copy"), group};
    const std::vector<MenuNode> layout{
        {.id = "action:a"},
        {.label = "Everything else", .items = {{.remaining = true}}},
        {.label = "Hidden", .items = {{.id = "unavailable"}}},
    };
    const auto items = rummage::views::arrange_menu(available, layout);
    REQUIRE(items.size() == 2);
    CHECK(items[0].id == "action:a");
    REQUIRE(items[1].children.size() == 2);
    CHECK(items[1].children[0].id == "copy");
    CHECK(ids(items[1].children[1].children) == Names{"action:b"});
}

TEST_CASE("context menu: pointer navigates nested panels and switches sibling groups") {
    rummage::ui::Offscreen gl{800, 400};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    ContextMenu menu{[] {}};
    int ran = 0;
    auto scripts = item("scripts");
    scripts.label = "Scripts";
    scripts.submenu = true;
    scripts.children = {item("run", true, [&] { ran = 1; })};
    scripts.children.front().label = "Run script";
    auto external = scripts;
    external.id = "external";
    external.label = "External";
    external.children.front().action = [&] { ran = 2; };
    auto root = item("my_actions");
    root.label = "My Actions";
    root.submenu = true;
    root.children = {scripts, external};
    const auto draw = [&] {
        renderer.begin_frame(800, 400, 1.0F, rummage::ui::Color::rgb(0x202020));
        menu.draw(renderer, {0, 0, 800, 400}, rummage::theme::Palette{});
        renderer.end_frame();
    };
    const auto pointer = [&](float x, float y, bool click = false) {
        rummage::ui::PointerEvent event;
        event.kind = click ? rummage::ui::PointerEvent::Kind::press
                           : rummage::ui::PointerEvent::Kind::motion;
        event.button = rummage::ui::PointerButton::left;
        event.x = x;
        event.y = y;
        CHECK(menu.pointer(event));
        draw();
    };
    menu.open(10, 10, {root}, false);
    draw();
    pointer(30, 30);
    CHECK(menu.open_levels() == 2);
    pointer(200, 30);
    CHECK(menu.open_levels() == 3);
    pointer(380, 30);
    CHECK(menu.highlighted(2) == 0);
    pointer(200, 58);  // switch to the sibling, discarding Scripts' open descendants
    CHECK(menu.open_levels() == 3);
    CHECK(menu.highlighted(1) == 1);
    pointer(380, 58, true);
    CHECK(ran == 2);
    CHECK_FALSE(menu.is_open());

    // At the bottom/right edge, panels flip left and overlapping panels hit the deepest one.
    menu.open(790, 390, {root}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    draw();
    pointer(620, 340, true);
    CHECK(ran == 1);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("properties: context menu and shortcut open a modal for files and the current folder") {
    Fixture f;
    f.select({"a.txt"});
    f.browser.open_menu(100, 100, true);
    f.choose("properties");
    auto& properties = f.browser.properties_dialog();
    REQUIRE(properties.is_open());
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos / "a.txt");
    CHECK_FALSE(properties.can_edit_icon());
    CHECK(f.browser.has_overlay());
    f.press(key(XKB_KEY_Delete));
    CHECK(fs::exists(f.photos / "a.txt"));
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(properties.is_open());
    f.select({});
    f.press(key(XKB_KEY_Return, {.alt = true}));
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos);
    CHECK(properties.can_edit_icon());
    properties.close();
    f.select({"a.txt", "b.txt"});
    f.press(key(XKB_KEY_Return, {.alt = true}));
    CHECK_FALSE(properties.is_open());
}

TEST_CASE(
    "properties: staging, cancel, apply and reset preserve metadata and refresh the selected "
    "folder") {
    Fixture f;
    f.select({"sub"});
    auto& properties = f.browser.properties_dialog();
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.choose_icon("folder-music");
    CHECK(properties.dirty());
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    properties.close();
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.pick_icon();
    REQUIRE(properties.picking());
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(properties.picking());
    CHECK(properties.is_open());
    properties.choose_icon("folder-music");
    // An external program changed another setting while Properties was open.
    std::ofstream{f.photos / "sub/.directory"} << "[Settings]\nView=3\n";
    properties.apply();
    REQUIRE(run_until(f.loop, [&] {
        const auto* entry = f.folder()->cursor_entry();
        return !properties.is_open() && entry && entry->folder_icon == "folder-music";
    }));
    CHECK(f.folder()->cursor_entry()->name == "sub");
    CHECK(read_file(f.photos / "sub/.directory").find("View=3") != std::string::npos);
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.choose_icon("");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] {
        const auto* entry = f.folder()->cursor_entry();
        return !properties.is_open() && entry && entry->folder_icon.empty();
    }));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=") == std::string::npos);
    CHECK(read_file(f.photos / "sub/.directory").find("View=3") != std::string::npos);
}

TEST_CASE("properties: failed saves stay open and do not recreate a removed folder") {
    Fixture f;
    f.select({"sub"});
    f.browser.show_properties();
    REQUIRE(f.settle());
    auto& properties = f.browser.properties_dialog();
    properties.choose_icon("/no/such/icon.png");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    CHECK(properties.is_open());
    CHECK_FALSE(properties.error().empty());
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    properties.choose_icon("folder-music");
    fs::remove(f.photos / "sub");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    CHECK(properties.is_open());
    CHECK_FALSE(properties.error().empty());
    CHECK_FALSE(fs::exists(f.photos / "sub"));
}

TEST_CASE("properties: obsolete metadata load cannot replace a newly opened item's properties") {
    Fixture f;
    auto& properties = f.browser.properties_dialog();
    properties.open(f.photos / "sub/", [] {});
    REQUIRE(f.settle());
    CHECK(properties.can_edit_icon());
    properties.open(f.photos / "sub", [] {});
    properties.open(f.photos / "a.txt", [] {});
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos / "a.txt");
    CHECK_FALSE(properties.can_edit_icon());
    properties.open(f.photos / "sub", [] {});
    properties.close();
    REQUIRE(f.settle());
    CHECK_FALSE(properties.is_open());
}

TEST_CASE("properties: keyboard search selects a themed icon and custom artwork stays staged") {
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(800, 640);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    Fixture f;
    const char* data_home = std::getenv("XDG_DATA_HOME");
    REQUIRE(data_home != nullptr);
    const auto theme = fs::path{data_home} / "icons/rummage-properties-test";
    fs::create_directories(theme / "48");
    std::ofstream{theme / "index.theme"} << "[Icon Theme]\nDirectories=48\n[48]\nSize=48\n";
    const std::string svg =
        "<svg width='32' height='32'><rect width='32' height='32' fill='red'/></svg>";
    std::ofstream{theme / "48/folder-red.svg"} << svg;
    std::ofstream{theme / "48/folder-blue.svg"} << svg;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    icons.set_theme("rummage-properties-test");
    f.select({"sub"});
    f.browser.show_properties();
    REQUIRE(f.settle());
    auto& properties = f.browser.properties_dialog();
    const auto draw = [&] {
        renderer.begin_frame(800, 640, 1, rummage::ui::Color::rgb(0));
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    f.press(key(XKB_KEY_Return));
    REQUIRE(properties.picking());
    draw();
    REQUIRE(f.settle());
    rummage::ui::KeyEvent query;
    query.text = "blue";
    f.press(query);
    draw();
    f.press(key(XKB_KEY_Tab));
    f.press(key(XKB_KEY_Return));
    CHECK_FALSE(properties.picking());
    CHECK(properties.icon() == "folder-blue");
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    f.press(key(XKB_KEY_Return));  // Apply is focused after choosing.
    REQUIRE(run_until(f.loop, [&] { return !properties.is_open(); }));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=folder-blue") != std::string::npos);
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.pick_icon();
    draw();
    f.press(key(XKB_KEY_Tab));
    f.press(key(XKB_KEY_Tab));  // custom path
    f.press(key(XKB_KEY_a, {.ctrl = true}));
    properties.paste((theme / "48/folder-red.svg").string());
    f.press(key(XKB_KEY_Return));
    CHECK(properties.icon() == (theme / "48/folder-red.svg").string());
    f.press(key(XKB_KEY_Escape));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=folder-blue") != std::string::npos);
}

TEST_CASE("properties: thumbnails replace the icon without moving the compact dialog") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "image/blocks.jpg", f.photos / "photo.jpg");
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto cache = std::make_shared<rummage::thumbs::Cache>(f.dir.path / "cache", "properties-test");
    rummage::views::Thumbnails thumbnails{f.loop, f.pool, cache, [] {}};
    auto& properties = f.browser.properties_dialog();
    properties.open(f.photos / "photo.jpg", [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    const auto draw = [&] {
        renderer.begin_frame(800, 640, 1, rummage::ui::Color::rgb(0));
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons, &thumbnails);
        renderer.end_frame();
        thumbnails.end_frame();
        std::vector<std::uint8_t> pixels(800 * 640 * 4);
        glReadPixels(0, 0, 800, 640, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto placeholder = draw();
    REQUIRE(thumbnails.loading());
    REQUIRE(run_until(f.loop, [&] { return !thumbnails.loading(); }));
    const auto preview = draw();
    CHECK(thumbnails.usage().textures == 1);
    CHECK(preview != placeholder);
    // Thumbnail completion changes only the header, keeping the metadata and footer still.
    CHECK(std::equal(preview.begin(), preview.begin() + 800 * 320 * 4, placeholder.begin()));
    // The content-sized footer is closer to the metadata than the old 520px-tall card.
    const auto card = properties.card_bounds();
    // Nix nests temporary files under a longer nix-shell directory. Allow that
    // location to widen the card, while short metadata should remain compact.
    if (fonts.layout(f.photos.string(), 12, renderer.scale()).width / renderer.scale() < 250)
        CHECK(card.width <= 460);
    CHECK(card.width <= 560);
    CHECK(card.width >= 320);
    CHECK(properties.cursor_at(card.right() - 182, card.bottom() - 39) ==
          rummage::ui::CursorShape::pointer);

    // Short windows retain a visible Close button while the content scrolls above it.
    renderer.begin_frame(800, 640, 1, rummage::ui::Color::rgb(0));
    properties.draw(renderer, {0, 0, 800, 300}, rummage::theme::Palette{}, icons, &thumbnails);
    renderer.end_frame();
    properties.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 400, .y = 150, .wheel_y = 500});
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = properties.card_bounds().right() - 182,
                        .y = properties.card_bounds().bottom() - 39,
                        .button = rummage::ui::PointerButton::left});
    CHECK_FALSE(properties.is_open());
}

TEST_CASE("properties: long filenames are bounded by the dialog and window") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    const auto file = f.photos / (std::string(140, 'x') + ".txt");
    std::ofstream{file} << "text";
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& properties = f.browser.properties_dialog();
    properties.open(file, [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    for (const float width : {800.0F, 320.0F}) {
        renderer.begin_frame(800, 640, 1, {});
        properties.draw(renderer, {0, 0, width, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
        const auto card = properties.card_bounds();
        CHECK(card.width == std::min(560.0F, width - 24));
        CHECK(card.x >= 12);
        CHECK(card.right() <= width - 12);
        CHECK(properties.cursor_at(card.right() - 182, card.bottom() - 39) ==
              rummage::ui::CursorShape::pointer);
    }
}

TEST_CASE("actions: missing archive tools remain actionable to explain installation") {
    Fixture f;
    f.dir.write("archive.zip", std::string{"PK\005\006"} + std::string(18, '\0'));
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.select({"archive.zip"});
    unsigned launched = 0;
    f.browser.set_action_handoff([&](const auto&, const auto&) { ++launched; },
                                 [](const auto&, const auto&) { return false; });
    f.browser.open_menu(100, 100, false);
    const auto item = std::ranges::find(f.menu().items(), "action:extract_here", &MenuItem::id);
    REQUIRE(item != f.menu().items().end());
    CHECK(item->enabled);
    CHECK(item->label.find("setup required") != std::string::npos);
    f.choose("action:extract_here");
    CHECK(launched == 1);
    const auto items = f.browser.palette_items();
    const auto to = std::ranges::find(items, "action:extract_to", &rummage::views::PaletteItem::id);
    REQUIRE(to != items.end());
    to->action();
    CHECK(launched == 2);
}

TEST_CASE("actions: errors remain queued and copyable when a later archive succeeds") {
    Fixture f;
    f.browser.show_action_result("First archive failed\nInstall unrar", true);
    f.browser.show_action_result("Second archive failed\nDisk full", true);
    f.browser.show_action_result("Extracted third archive", false);
    auto& errors = f.browser.action_errors();
    CHECK(errors.pending() == 2);
    CHECK(errors.detail() == "First archive failed\nInstall unrar");
    CHECK(f.browser.has_overlay());
    errors.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(errors.pending() == 1);
    CHECK(errors.detail() == "Second archive failed\nDisk full");
    errors.key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK_FALSE(errors.is_open());
}

TEST_CASE("properties: wrapped names and metadata support selecting and copying exact text") {
    rummage::ui::Offscreen gl{800, 900};
    Fixture f;
    const std::string name = "résumé " + std::string(160, 'w') + ".txt";
    const auto file = f.photos / name;
    std::ofstream{file} << "hello";
    auto& properties = f.browser.properties_dialog();
    std::string copied;
    properties.set_copy_handler([&](std::string value) { copied = std::move(value); });
    properties.open(file, [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&](float width = 400) {
        renderer.begin_frame(800, 900, 1, rummage::ui::Color::rgb(0));
        properties.draw(renderer, {0, 0, width, 900}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    const auto press = [&](const rummage::ui::Rect& rect, int clicks = 1) {
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = rect.x + 1,
                            .y = rect.y + 2,
                            .button = rummage::ui::PointerButton::left,
                            .click_count = clicks});
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                            .x = rect.x + 1,
                            .y = rect.y + 2,
                            .button = rummage::ui::PointerButton::left});
    };
    draw();
    const auto wrapped = properties.text_bounds("name");
    CHECK(wrapped.height > 3 * 22);
    CHECK(wrapped.right() <= properties.card_bounds().right() - 24);
    CHECK(properties.text_bounds("Location").y >= wrapped.bottom());
    CHECK(properties.cursor_at(wrapped.x + 1, wrapped.y + 2) == rummage::ui::CursorShape::text);

    // A double click selects a Unicode word using the displayed proportional font.
    press(wrapped, 2);
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == "résumé");
    // Leaving the surface ends an unfinished drag without discarding its selection.
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = wrapped.x + 1,
                        .y = wrapped.y + 2,
                        .button = rummage::ui::PointerButton::left,
                        .click_count = 2});
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::leave});
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::motion,
                        .x = wrapped.right(),
                        .y = wrapped.bottom()});
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == "résumé");
    properties.key(key(XKB_KEY_a, {.ctrl = true}));
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == name);  // soft wraps never become clipboard newlines

    // Resizing preserves source offsets, while every character remains readable by wrapping.
    draw(320);
    CHECK(properties.text_bounds("name").height > wrapped.height);
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == name);

    const auto location = properties.text_bounds("Location");
    press(location, 3);
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == f.photos.string());
    const auto size = properties.text_bounds("Size");
    press(size);
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = size.x,
                        .y = size.y + 2,
                        .button = rummage::ui::PointerButton::left,
                        .click_count = 1});
    properties.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::motion, .x = size.right() + 20, .y = size.y + 2});
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                        .x = size.right() + 20,
                        .y = size.y + 2,
                        .button = rummage::ui::PointerButton::left});
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == "5 bytes (5 bytes)");

    // An empty click clears focus, and reopening another item cannot copy the previous field.
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = 1,
                        .y = 1,
                        .button = rummage::ui::PointerButton::left});
    copied.clear();
    properties.key(key(XKB_KEY_a, {.ctrl = true}));
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied.empty());
    properties.open(f.photos / "a.txt", [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    draw();
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied.empty());
}

TEST_CASE("properties: selecting a visually placed RTL character copies its source bytes") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& properties = f.browser.properties_dialog();
    std::string copied;
    properties.set_copy_handler([&](std::string value) { copied = std::move(value); });
    for (const std::string name : {"אבג", "عربي"}) {
        std::ofstream{f.photos / name} << "text";
        properties.open(f.photos / name, [] {});
        REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
        renderer.begin_frame(800, 640, 1, {});
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
        const auto rect = properties.text_bounds("name");
        const auto spans = rummage::views::SelectableText::glyph_spans(fonts, name, 16, 1);
        REQUIRE_FALSE(spans.empty());
        const auto& first = spans.front();
        REQUIRE(first.rtl);
        CHECK(first.left > 0);
        const auto pixel = [&](const rummage::views::SelectableText::GlyphSpan& span) {
            std::vector<std::uint8_t> rgba(4);
            glReadPixels(static_cast<int>(rect.x + (span.left + span.right) / 2),
                         639 - static_cast<int>(rect.y + 20), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
            return rgba;
        };
        const auto before_selected = pixel(first);
        const auto before_other = pixel(spans.back());
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = rect.x + first.left + 0.1F,
                            .y = rect.y + 2,
                            .button = rummage::ui::PointerButton::left,
                            .click_count = 1});
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::motion,
                            .x = rect.x + first.right - 0.1F,
                            .y = rect.y + 2});
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                            .x = rect.x + first.right - 0.1F,
                            .y = rect.y + 2,
                            .button = rummage::ui::PointerButton::left});
        properties.key(key(XKB_KEY_c, {.ctrl = true}));
        CHECK(copied == name.substr(first.begin, first.end - first.begin));
        renderer.begin_frame(800, 640, 1, {});
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
        CHECK(pixel(first) != before_selected);
        CHECK(pixel(spans.back()) == before_other);
    }
}

TEST_CASE("properties: Tab from selected metadata begins the button focus order") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& properties = f.browser.properties_dialog();
    properties.open(f.photos / "sub", [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    const auto select_name = [&] {
        renderer.begin_frame(800, 640, 1, {});
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
        const auto name = properties.text_bounds("name");
        properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = name.x + 1,
                            .y = name.y + 2,
                            .button = rummage::ui::PointerButton::left});
    };
    select_name();
    properties.key(key(XKB_KEY_Tab));
    properties.key(key(XKB_KEY_Return));
    CHECK(properties.picking());  // Change Icon is first, before Default.
    properties.key(key(XKB_KEY_Escape));
    properties.choose_icon("folder-music");
    select_name();
    properties.key(key(XKB_KEY_Tab, {.shift = true}));
    properties.key(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    CHECK_FALSE(properties.is_open());  // Apply is last.
    CHECK(read_file(f.photos / "sub/.directory").find("folder-music") != std::string::npos);
}

TEST_CASE("selectable text: exact-fit labels and natural wraps retain source bytes") {
    rummage::ui::Offscreen gl{800, 640};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::views::SelectableText text;
    for (const float scale : {1.0F, 1.25F, 1.5F, 1.75F}) {
        renderer.begin_frame(800, 640, scale, {});
        const float width = fonts.layout("Permissions", 12, scale).width / scale;
        CHECK(rummage::views::SelectableText::height(renderer, "Permissions", width, 12) == 18);
        // A subpixel accumulation difference must not put the final 's' on a second line.
        CHECK(rummage::views::SelectableText::height(renderer, "Permissions", width - 0.001F, 12) ==
              18);
        renderer.end_frame();
    }

    struct Wrap {
        std::string value, fits, first_line;
    };

    for (const auto& wrap : std::vector<Wrap>{{"filename with words", "filename wi", "filename "},
                                              {"folder/subfolder/file", "folder/sub", "folder/"},
                                              {"prefix-longfilename", "prefix-lo", "prefix-"},
                                              {"today at 22:54", "today at 22:5", "today at "}}) {
        renderer.begin_frame(800, 640, 1, {});
        const float width = fonts.layout(wrap.fits, 12, 1).width;
        text.begin_frame({0, 0, 800, 640});
        text.draw(renderer, "value", wrap.value, {20, 20, width, 0}, 12,
                  rummage::ui::Color::rgb(0xFFFFFF), rummage::ui::Color::rgb(0x336699));
        text.end_frame();
        renderer.end_frame();
        text.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                      .x = 20,
                      .y = 22,
                      .button = rummage::ui::PointerButton::left,
                      .click_count = 1});
        text.pointer(
            {.kind = rummage::ui::PointerEvent::Kind::motion, .x = 20 + width + 10, .y = 22});
        text.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                      .x = 20 + width + 10,
                      .y = 22,
                      .button = rummage::ui::PointerButton::left});
        CHECK(text.selected_text() == wrap.first_line);
        text.select_all();
        CHECK(text.selected_text() == wrap.value);
    }
}

TEST_CASE(
    "context menu: Allow Running as Program, Stop, undone and redone; only where it means "
    "something") {
    Fixture f;
    const fs::path app = f.photos / "Tool.AppImage";
    std::ofstream{app} << "x";
    ::chmod(app.c_str(), 0644);
    const auto mode = [&app] {
        struct stat info{};
        REQUIRE(::stat(app.c_str(), &info) == 0);
        return info.st_mode & 07777U;
    };
    const auto listed_mode = [&f] {
        for (const rummage::fs::Entry& entry : f.folder()->entries()) {
            if (entry.name == "Tool.AppImage") {
                return static_cast<unsigned>(entry.mode);
            }
        }
        return 0U;
    };
    REQUIRE(run_until(f.loop, [&] { return listed_mode() == 0644; }));
    const auto label = [&f](const std::string& id) {
        for (const MenuItem& item : f.menu().items()) {
            if (item.id == id) {
                return item.label;
            }
        }
        return std::string{};
    };

    // An AppImage that may not run: "Allow".
    f.select({"Tool.AppImage"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(label("running") == "Allow Running as Program");
    f.choose("running");
    REQUIRE(run_until(f.loop, [&] { return listed_mode() == 0744; }));
    CHECK(mode() == 0744U);
    CHECK(f.browser.status_bar().message() == "“Tool.AppImage” can run as a program");

    // Now it may: "Stop"; undone and redone with the keys.
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(label("running") == "Stop Running as Program");
    f.menu().close();
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    REQUIRE(run_until(f.loop, [&] { return listed_mode() == 0644; }));
    CHECK(f.browser.status_bar().message() ==
          "Undone: “Tool.AppImage” can no longer run as a program");
    CHECK(f.press(key(XKB_KEY_Z, {.ctrl = true, .shift = true})));
    REQUIRE(run_until(f.loop, [&] { return listed_mode() == 0744; }));
    CHECK(f.browser.status_bar().message() == "Redone: “Tool.AppImage” can run as a program");

    // A text file: not offered; one that may run (copied from a FAT stick): "Stop" is.
    f.select({"a.txt"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(label("running").empty());
    f.menu().close();
    ::chmod((f.photos / "a.txt").c_str(), 0755);
    REQUIRE(run_until(f.loop, [&] {
        const auto& entries = f.folder()->entries();
        const auto it = std::ranges::find(entries, "a.txt", &rummage::fs::Entry::name);
        return it != entries.end() && it->mode == 0755;
    }));
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(label("running") == "Stop Running as Program");
    f.menu().close();

    // With a folder among them: never.
    f.select({"Tool.AppImage", "sub"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(label("running").empty());
    f.menu().close();
}

TEST_CASE("ask to run: the card names the program, and only Allow and Run lets it run") {
    Fixture f;
    int allowed = 0;
    f.browser.ask_to_run(f.photos / "Tool.AppImage", [&allowed] { ++allowed; });
    REQUIRE(f.browser.confirm_dialog().is_open());
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(f.browser.confirm_dialog().is_open());
    CHECK(allowed == 0);
    f.browser.ask_to_run(f.photos / "Tool.AppImage", [&allowed] { ++allowed; });
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(allowed == 1);
}

TEST_CASE("i18n: templates and custom actions keep their names beside translated commands") {
    struct RestoreEnglish {
        ~RestoreEnglish() { rummage::i18n::set_language("en"); }
    } restore;

    rummage::i18n::set_language("de");
    Fixture f;
    const fs::path templates = f.dir.path / "templates";
    fs::create_directories(templates);
    std::ofstream{templates / "Home"} << "literal template";
    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.settle());
    const auto make = std::ranges::find(f.menu().items(), "new_file", &MenuItem::id);
    REQUIRE(make != f.menu().items().end());
    const auto named = std::ranges::find(make->children, "template:Home", &MenuItem::id);
    REQUIRE(named != make->children.end());
    CHECK(named->label == "Home");
    const auto create = named->action;
    f.menu().close();
    create();
    REQUIRE(f.settle());
    CHECK(read_file(f.photos / "Home") == "literal template");
    f.browser.name_editor().cancel();
    using rummage::core::Action;
    const std::vector<Action> custom{{.id = "literal",
                                      .label = "Home",
                                      .argv = {"true"},
                                      .mime = {},
                                      .selection = Action::Selection::any,
                                      .placement = Action::Placement::menu}};
    f.browser.set_actions(custom);
    f.browser.set_action_handoff([](const auto&, const auto&) {},
                                 [](const auto&, const auto&) { return true; });
    f.select({"a.txt"});
    f.browser.open_menu(100, 100, false);
    const auto action = std::ranges::find(f.menu().items(), "action:literal", &MenuItem::id);
    REQUIRE(action != f.menu().items().end());
    CHECK(action->label == "Home");
    const auto open = std::ranges::find(f.menu().items(), "open", &MenuItem::id);
    REQUIRE(open != f.menu().items().end());
    CHECK(open->label == "Öffnen");
    const auto palette = f.browser.palette_items();
    const auto command =
        std::ranges::find(palette, "action:literal", &rummage::views::PaletteItem::id);
    REQUIRE(command != palette.end());
    CHECK(command->label == "Home");
}

TEST_CASE("context menu: tall menus scroll and keep the keyboard selection reachable") {
    namespace ui = rummage::ui;
    ui::Offscreen surface{320, 200};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ContextMenu menu{[] {}};
    int chosen = -1;
    std::vector<MenuItem> items;
    for (int i = 0; i < 80; ++i)
        items.push_back(item(std::to_string(i), true, [&, i] { chosen = i; }));
    menu.open(280, 190, std::move(items), true);
    const auto draw = [&] {
        renderer.begin_frame(320, 200, 1, {});
        menu.draw(renderer, {0, 0, 320, 200}, rummage::theme::Palette{});
        renderer.end_frame();
    };
    draw();
    SUBCASE("End reveals the last row") {
        menu.key(key(XKB_KEY_End));
        CHECK(menu.highlighted() == 79);
    }
    SUBCASE("wheel reaches the last row") {
        menu.pointer({.kind = ui::PointerEvent::Kind::scroll, .x = 100, .y = 60, .wheel_y = 1000});
    }
    SUBCASE("scrollbar retains capture outside the panel") {
        menu.pointer({.kind = ui::PointerEvent::Kind::press,
                      .x = 269,
                      .y = 20,
                      .button = ui::PointerButton::left});
        menu.pointer({.kind = ui::PointerEvent::Kind::motion, .x = 400, .y = 400});
        menu.pointer({.kind = ui::PointerEvent::Kind::release,
                      .x = 400,
                      .y = 400,
                      .button = ui::PointerButton::left});
    }
    SUBCASE("page navigation advances by a viewport") {
        menu.key(key(XKB_KEY_Page_Down));
        CHECK(menu.highlighted() > 1);
        CHECK(menu.highlighted() < 10);
        menu.key(key(XKB_KEY_End));
    }
    draw();
    // Padding is not a clipped row's hit target.
    menu.pointer({.kind = ui::PointerEvent::Kind::press,
                  .x = 100,
                  .y = 5,
                  .button = ui::PointerButton::left});
    CHECK(chosen == -1);
    menu.pointer({.kind = ui::PointerEvent::Kind::press,
                  .x = 100,
                  .y = 176,
                  .button = ui::PointerButton::left});
    CHECK(chosen == 79);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("context menu: scrolling a parent closes its child and restores keyboard routing") {
    namespace ui = rummage::ui;
    ui::Offscreen surface{640, 240};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ContextMenu menu{[] {}};
    MenuItem parent = item("Applications");
    parent.submenu = true;
    parent.children = {item("First app"), item("Second app")};
    std::vector<MenuItem> items{parent};
    for (int i = 0; i < 40; ++i)
        items.push_back(item(std::to_string(i)));
    menu.open(10, 10, std::move(items), true);
    const auto draw = [&] {
        renderer.begin_frame(640, 240, 1, {});
        menu.draw(renderer, {0, 0, 640, 240}, rummage::theme::Palette{});
        renderer.end_frame();
    };
    draw();
    menu.key(key(XKB_KEY_Right));
    draw();
    REQUIRE(menu.submenu_open());
    menu.pointer({.kind = ui::PointerEvent::Kind::scroll, .x = 30, .y = 100, .wheel_y = 10});
    CHECK_FALSE(menu.submenu_open());
    menu.key(key(XKB_KEY_End));
    draw();
    CHECK(menu.highlighted() == 40);
    menu.key(key(XKB_KEY_Home));
    draw();
    CHECK(menu.highlighted() == 0);
    menu.key(key(XKB_KEY_Right));
    draw();
    CHECK(menu.submenu_open());
}

TEST_CASE("properties: embedded image metadata wraps, scrolls and copies in a narrow dialog") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    const std::string value = "Author-supplied metadata " + std::string(350, 'x');
    const auto png = rummage::picture::encode_png(
        {.width = 1, .height = 1, .rgba = {10, 20, 30, 255}}, {{"Last custom tag", value}});
    const auto file =
        f.dir.write("metadata.png", {reinterpret_cast<const char*>(png.data()), png.size()});
    auto& properties = f.browser.properties_dialog();
    properties.open(file, [] {});
    REQUIRE(run_until(f.loop, [&] { return !properties.loading(); }));
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 640, 1, rummage::ui::Color::rgb(0));
        properties.draw(renderer, {0, 0, 360, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    const auto disclosure = properties.control_bounds("details");
    REQUIRE_FALSE(disclosure.empty());
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = disclosure.x + 8,
                        .y = disclosure.y + 8,
                        .button = rummage::ui::PointerButton::left});
    draw();
    properties.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 180, .y = 300, .wheel_y = 10000});
    draw();
    const auto rect = properties.text_bounds("PNG · Last custom tag");
    REQUIRE_FALSE(rect.empty());
    CHECK(rect.height > 26);
    CHECK(rect.x >= properties.card_bounds().x);
    CHECK(rect.right() <= properties.card_bounds().right());
    std::string copied;
    properties.set_copy_handler([&](std::string text) { copied = std::move(text); });
    properties.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = rect.x + 2,
                        .y = std::max(rect.y + 2, 100.0F),
                        .button = rummage::ui::PointerButton::left});
    properties.key(key(XKB_KEY_a, {.ctrl = true}));
    properties.key(key(XKB_KEY_c, {.ctrl = true}));
    CHECK(copied == value);
}

TEST_CASE("properties: permissions are staged, cancellable and applied to files and directories") {
    Fixture f;
    for (const auto name : {"a.txt", "sub"}) {
        const auto path = f.photos / name;
        REQUIRE(::chmod(path.c_str(), 0755) == 0);
        f.select({name});
        f.browser.show_properties();
        REQUIRE(f.settle());
        auto& dialog = f.browser.properties_dialog();
        REQUIRE(dialog.can_edit_permissions());
        CHECK(dialog.permissions() == 0755);
        dialog.set_permissions(0700);
        CHECK(dialog.dirty());
        struct stat info{};
        REQUIRE(::stat(path.c_str(), &info) == 0);
        CHECK((info.st_mode & 0777) == 0755);
        dialog.close();
        f.browser.show_properties();
        REQUIRE(f.settle());
        CHECK(dialog.permissions() == 0755);
        CHECK_FALSE(dialog.dirty());
        dialog.set_permissions(0750);
        dialog.apply();
        REQUIRE(run_until(f.loop, [&] { return !dialog.saving(); }));
        CHECK_FALSE(dialog.is_open());
        REQUIRE(::stat(path.c_str(), &info) == 0);
        CHECK((info.st_mode & 0777) == 0750);
    }
}

TEST_CASE("properties: permission errors stay visible and read-only contexts cannot edit") {
    Fixture f;
    const auto path = f.photos / "a.txt";
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    auto& dialog = f.browser.properties_dialog();
    dialog.open(path, [] {});
    REQUIRE(f.settle());
    dialog.set_permissions(0755);
    REQUIRE(::chmod(path.c_str(), 0600) == 0);
    dialog.apply();
    REQUIRE(run_until(f.loop, [&] { return !dialog.saving(); }));
    CHECK(dialog.is_open());
    CHECK_FALSE(dialog.error().empty());
    dialog.close();
    dialog.open(path, [] {}, false);
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.can_edit_permissions());
    dialog.set_permissions(0777);
    CHECK_FALSE(dialog.dirty());
    fs::create_symlink(path, f.photos / "link");
    dialog.open(f.photos / "link", [] {});
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.can_edit_permissions());
}

TEST_CASE("properties: every rwx permission toggle is keyboard accessible") {
    Fixture f;
    const auto path = f.photos / "a.txt";
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    auto& dialog = f.browser.properties_dialog();
    dialog.open(path, [] {});
    REQUIRE(f.settle());
    std::uint16_t mode = 0644;
    // Close initially has focus. Skip Copy, then visit Others/execute through Owner/read.
    dialog.key(key(XKB_KEY_Tab, {.shift = true}));
    for (unsigned bit = 0; bit < 9; ++bit) {
        dialog.key(key(XKB_KEY_Tab, {.shift = true}));
        dialog.key(key(XKB_KEY_space));
        mode = static_cast<std::uint16_t>(mode ^ (1U << bit));
        CHECK(dialog.permissions() == mode);
    }
    dialog.close();
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0644);
}

TEST_CASE("properties: applying an icon before removing directory write access") {
    Fixture f;
    const auto path = f.photos / "sub";
    REQUIRE(::chmod(path.c_str(), 0755) == 0);
    auto& dialog = f.browser.properties_dialog();
    dialog.open(path, [] {});
    REQUIRE(f.settle());
    dialog.choose_icon("folder-music");
    dialog.set_permissions(0500);
    dialog.apply();
    REQUIRE(run_until(f.loop, [&] { return !dialog.saving(); }));
    CHECK_FALSE(dialog.is_open());
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0500);
    CHECK(read_file(path / ".directory").find("folder-music") != std::string::npos);
    REQUIRE(::chmod(path.c_str(), 0755) == 0);  // allow temporary-directory cleanup
}

TEST_CASE("properties: wheel, touchpad and a draggable scrollbar navigate long metadata") {
    rummage::ui::Offscreen gl{800, 640};
    Fixture f;
    const auto png =
        rummage::picture::encode_png({.width = 1, .height = 1, .rgba = {10, 20, 30, 255}},
                                     {{"Long tag", std::string(1200, 'x')}});
    const auto file =
        f.dir.write("metadata.png", {reinterpret_cast<const char*>(png.data()), png.size()});
    auto& dialog = f.browser.properties_dialog();
    dialog.open(file, [] {}, false);
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 640, 1, {});
        dialog.draw(renderer, {0, 0, 360, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    const auto details = dialog.control_bounds("details");
    REQUIRE_FALSE(details.empty());
    dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                    .x = details.x + 4,
                    .y = details.y + 4,
                    .button = rummage::ui::PointerButton::left});
    draw();
    REQUIRE(dialog.scroll_offset() == 0);
    SUBCASE("wheel notches move rows and touchpad pixels remain precise") {
        dialog.pointer(
            {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 180, .y = 300, .wheel_y = 1});
        CHECK(dialog.scroll_offset() == doctest::Approx(48));
        dialog.pointer(
            {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 180, .y = 300, .wheel_y = 0.5F});
        CHECK(dialog.scroll_offset() == doctest::Approx(72));
        dialog.pointer(
            {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 180, .y = 300, .scroll_y = 18});
        CHECK(dialog.scroll_offset() == doctest::Approx(90));
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::scroll,
                        .x = 180,
                        .y = 300,
                        .wheel_y = -1000});
        CHECK(dialog.scroll_offset() == 0);
    }
    SUBCASE("wide hit area supports dragging outside the dialog and releases capture") {
        const auto thumb = dialog.scrollbar_bounds();
        REQUIRE(thumb.width >= 6);
        REQUIRE(thumb.height >= 24);
        const float x = thumb.x - 2;  // within the hit strip, outside the visible thumb
        CHECK(dialog.cursor_at(x, thumb.y + 5) == rummage::ui::CursorShape::pointer);
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = x,
                        .y = thumb.y + 5,
                        .button = rummage::ui::PointerButton::left});
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 790, .y = 630});
        CHECK(dialog.scroll_offset() > 200);
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                        .x = 790,
                        .y = 630,
                        .button = rummage::ui::PointerButton::left});
        const auto at_end = dialog.scroll_offset();
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = x, .y = thumb.y});
        CHECK(dialog.scroll_offset() == at_end);
    }
    SUBCASE("track clicks jump through the document") {
        const auto thumb = dialog.scrollbar_bounds();
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = thumb.x + 2,
                        .y = dialog.card_bounds().bottom() - 90,
                        .button = rummage::ui::PointerButton::left});
        CHECK(dialog.scroll_offset() > 200);
    }
}

TEST_CASE("properties: media disclosure and top copy include hidden metadata") {
    rummage::ui::Offscreen gl{800, 900};
    Fixture f;
    const std::string value = "Full embedded value " + std::string(400, 'x');
    const auto png = rummage::picture::encode_png(
        {.width = 1, .height = 1, .rgba = {10, 20, 30, 255}}, {{"Custom tag", value}});
    const auto file =
        f.dir.write("metadata.png", {reinterpret_cast<const char*>(png.data()), png.size()});
    auto& dialog = f.browser.properties_dialog();
    std::string copied;
    dialog.set_copy_handler([&](std::string text) { copied = std::move(text); });
    dialog.open(file, [] {}, false);
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(800, 900, 1, {});
        dialog.draw(renderer, {0, 0, 420, 900}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    const auto click = [&](std::string_view id) {
        const auto rect = dialog.control_bounds(id);
        REQUIRE_FALSE(rect.empty());
        CHECK(dialog.cursor_at(rect.x + 4, rect.y + 4) == rummage::ui::CursorShape::pointer);
        dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                        .x = rect.x + 4,
                        .y = rect.y + 4,
                        .button = rummage::ui::PointerButton::left});
        draw();
    };
    draw();
    CHECK_FALSE(dialog.text_bounds("Dimensions").empty());
    CHECK(dialog.text_bounds("Aspect ratio").empty());
    CHECK(dialog.text_bounds("PNG · Custom tag").empty());
    CHECK(dialog.control_bounds("copy").y < dialog.text_bounds("name").y);
    const float collapsed_height = dialog.card_bounds().height;
    click("copy");
    CHECK(copied.find("Name: metadata.png\n") != std::string::npos);
    CHECK(copied.find("Type: image/png\n") != std::string::npos);
    CHECK(copied.find("Location: " + file.parent_path().string()) != std::string::npos);
    CHECK(copied.find("Dimensions: 1 × 1\n") != std::string::npos);
    CHECK(copied.find("Aspect ratio: 1:1\n") != std::string::npos);
    CHECK(copied.find("PNG · Custom tag: " + value + "\n") != std::string::npos);
    const auto all = copied;
    click("details");
    CHECK_FALSE(dialog.text_bounds("Aspect ratio").empty());
    CHECK_FALSE(dialog.text_bounds("PNG · Custom tag").empty());
    CHECK(dialog.card_bounds().height > collapsed_height);
    dialog.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 210, .y = 450, .wheel_y = 10000});
    draw();
    CHECK(dialog.control_bounds("copy").y == dialog.card_bounds().y + 12);
    click("copy");
    CHECK(copied == all);
    // Reverse Tab from Copy reaches the disclosure and scrolls it into view.
    dialog.key(key(XKB_KEY_Tab, {.shift = true}));
    draw();
    dialog.key(key(XKB_KEY_space));
    draw();
    CHECK(dialog.text_bounds("PNG · Custom tag").empty());
    CHECK(dialog.card_bounds().height == collapsed_height);
    dialog.key(key(XKB_KEY_Return));
    draw();
    CHECK_FALSE(dialog.text_bounds("PNG · Custom tag").empty());
    dialog.open(file, [] {}, false);
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    draw();
    CHECK(dialog.text_bounds("PNG · Custom tag").empty());
    dialog.open(f.photos / "a.txt", [] {}, false);
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    draw();
    CHECK(dialog.control_bounds("details").empty());
    click("copy");
    CHECK(copied.find("Name: a.txt\n") != std::string::npos);
    CHECK(copied.find("Custom tag") == std::string::npos);
}

TEST_CASE("context menu: Create Link makes a link next to each item, and selects them") {
    Fixture f;
    f.select({"a.txt", "sub"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("create_link");
    REQUIRE(f.settle());
    CHECK(fs::read_symlink(f.photos / "Link to a.txt") == f.photos / "a.txt");
    CHECK(fs::read_symlink(f.photos / "Link to sub") == f.photos / "sub");
    REQUIRE(run_until(f.loop, [&] {
        Names selected = f.folder()->selected_names();
        std::ranges::sort(selected);
        return selected == Names{"Link to a.txt", "Link to sub"};
    }));
}

TEST_CASE("properties: drive facts load without directory totals and appear in copy-all") {
    Fixture f;
    rummage::ui::Offscreen gl{800, 900};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& dialog = f.browser.properties_dialog();
    std::string copied;
    dialog.set_copy_handler([&](std::string value) { copied = std::move(value); });
    dialog.open_drive(f.photos, "Test drive");
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    REQUIRE_FALSE(dialog.filesystem().error);
    CHECK(dialog.filesystem().capacity > 0);
    CHECK_FALSE(dialog.can_edit_icon());
    CHECK_FALSE(dialog.can_edit_permissions());
    renderer.begin_frame(800, 900, 1, {});
    dialog.draw(renderer, {0, 0, 800, 900}, {}, icons);
    renderer.end_frame();
    CHECK_FALSE(dialog.text_bounds("Capacity").empty());
    CHECK_FALSE(dialog.text_bounds("Available").empty());
    CHECK(dialog.text_bounds("Contents").empty());
    const auto copy = dialog.control_bounds("copy");
    dialog.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                    .x = copy.x + 2,
                    .y = copy.y + 2,
                    .button = rummage::ui::PointerButton::left});
    CHECK(copied.find("Capacity") != std::string::npos);
    CHECK(copied.find("Mount point") != std::string::npos);
    dialog.open_drive(f.photos / "missing", "Missing");
    REQUIRE(run_until(f.loop, [&] { return !dialog.loading(); }));
    CHECK_FALSE(dialog.error().empty());
    dialog.open_drive(f.photos, "Test drive");
    dialog.close();
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.is_open());
    dialog.open(f.photos / "a.txt", [] {});
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.can_edit_icon());
    CHECK(dialog.filesystem().capacity == 0);
}

TEST_CASE("context menu: mounted drives offer properties even without udisks") {
    Fixture f;
    rummage::ui::Offscreen gl{800, 640};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& sidebar = f.browser.sidebar();
    for (const bool mounted : {true, false}) {
        sidebar.set_places({{.section = rummage::fs::Place::Section::devices,
                             .label = "Test drive",
                             .path = mounted ? f.photos : fs::path{},
                             .icon = "server",
                             .disk = {.volume = {}, .mounted = mounted}}});
        renderer.begin_frame(800, 640, 1, {});
        sidebar.draw(renderer, {0, 0, 200, 640}, {}, icons, f.photos, Browser::Clock::now());
        renderer.end_frame();
        float y = 0;
        for (; y < 640; y += 2) {
            sidebar.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 30, .y = y},
                            Browser::Clock::now());
            if (sidebar.hovered_place())
                break;
        }
        REQUIRE(y < 640);
        sidebar.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                         .x = 30,
                         .y = y,
                         .button = rummage::ui::PointerButton::right},
                        Browser::Clock::now());
        REQUIRE(f.menu().is_open());
        const auto properties = std::ranges::find(f.menu().items(), "properties", &MenuItem::id);
        REQUIRE(properties != f.menu().items().end());
        CHECK(properties->enabled == mounted);
        if (mounted) {
            f.choose("properties");
            REQUIRE(run_until(f.loop, [&] { return !f.browser.properties_dialog().loading(); }));
            CHECK(f.browser.properties_dialog().filesystem().capacity > 0);
            f.browser.properties_dialog().close();
        } else
            f.menu().close();
    }
}

TEST_CASE("sidebar: places and bookmarks open the clicked folder in a new tab") {
    Fixture f;
    SUBCASE("built-in layout") {}
    SUBCASE("legacy saved layout") {
        const auto config = rummage::core::parse_config(R"(
[context_menu]
sidebar = [
  { id = "mount" },
  { id = "unmount" },
  { id = "eject", separator = true },
  { id = "remove_bookmark" },
  { id = "empty_trash" },
]
)",
                                                        "legacy.toml");
        REQUIRE(config.problems.empty());
        f.browser.set_context_menu(config.config.context_menu);
    }
    rummage::ui::Offscreen gl{800, 640};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    auto& sidebar = f.browser.sidebar();
    for (const auto section :
         {rummage::fs::Place::Section::places, rummage::fs::Place::Section::bookmarks}) {
        f.browser.open(f.photos);
        REQUIRE(f.settle());
        sidebar.set_places({{.section = section,
                             .label = "Target",
                             .path = f.photos / "sub",
                             .icon = "folder",
                             .disk = {}}});
        renderer.begin_frame(800, 640, 1, {});
        sidebar.draw(renderer, {0, 0, 200, 640}, {}, icons, f.photos, Browser::Clock::now());
        renderer.end_frame();
        float y = 0;
        for (; y < 640; y += 2) {
            sidebar.pointer({.kind = rummage::ui::PointerEvent::Kind::motion, .x = 30, .y = y},
                            Browser::Clock::now());
            if (sidebar.hovered_place())
                break;
        }
        REQUIRE(y < 640);
        sidebar.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                         .x = 30,
                         .y = y,
                         .button = rummage::ui::PointerButton::right},
                        Browser::Clock::now());
        REQUIRE(f.menu().is_open());
        f.choose("open_tab");
        REQUIRE(f.settle());
        CHECK(f.browser.picker_folder()->path() == f.photos / "sub");
        const auto state = f.browser.state();
        CHECK(state.tabs.size() >= 2);
    }
}

TEST_CASE("content files: atomic publication never overwrites and cancellation leaves nothing") {
    const TempDir dir;
    const auto make = [&](std::string_view name, const rummage::core::CancelToken& cancel = {}) {
        return rummage::fs::make_contents_file(dir.path, name, "complete contents", cancel);
    };
    std::ofstream{dir.path / "original"} << "keep";
    fs::create_symlink("original", dir.path / "Saved.txt");
    REQUIRE(make("Saved.txt").name == "Saved 2.txt");
    CHECK(read_file(dir.path / "original") == "keep");
    CHECK(read_file(dir.path / "Saved 2.txt") == "complete contents");
    CHECK((fs::status(dir.path / "Saved 2.txt").permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
    rummage::core::CancelToken cancel;
    cancel.cancel();
    CHECK(make("Cancelled.txt", cancel).error == std::errc::operation_canceled);
    CHECK(make("../escape").error == std::errc::invalid_argument);
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i)
        writers.emplace_back([&] { CHECK_FALSE(make("Saved.txt").error); });
    for (auto& writer : writers)
        writer.join();
    CHECK(std::distance(fs::directory_iterator{dir.path}, fs::directory_iterator{}) == 11);
    for (const auto& entry : fs::directory_iterator{dir.path}) {
        CHECK_FALSE(entry.path().filename().string().starts_with(".rummage-content-"));
        if (!entry.is_symlink() && entry.path().filename() != "original")
            CHECK(read_file(entry.path()) == "complete contents");
    }
}

TEST_CASE("content files: partial write failures remove all staging output") {
    const TempDir dir;
    // Limit only this child: write(2) succeeds partially, then fails with EFBIG.
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        const rlimit limit{1024, 1024};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
            _exit(2);
        const auto made =
            rummage::fs::make_contents_file(dir.path, "partial.txt", std::string(8192, 'x'), {});
        _exit(made.error == std::errc::file_too_large ? 0 : 1);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(fs::is_empty(dir.path));
}
