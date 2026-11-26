#include "rummage/fs/places.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/sidebar.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::fs::Place;
using rummage::fs::PlaceSources;
using rummage::fs::UserDir;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::place_holding;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool run_until(EventLoop& loop, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 5s) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(limit, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

std::vector<std::string> labels(const std::vector<Place>& places) {
    std::vector<std::string> out;
    out.reserve(places.size());
    for (const Place& place : places) {
        out.push_back(place.label);
    }
    return out;
}

rummage::ui::KeyEvent key(xkb_keysym_t keysym, bool ctrl = false) {
    rummage::ui::KeyEvent event;
    event.keysym = keysym;
    event.modifiers.ctrl = ctrl;
    return event;
}

// A home with Pictures and Downloads, a config dir, an empty mount table.
struct Home {
    TempDir dir;
    PlaceSources sources{.home = dir.path / "home",
                         .config_home = dir.path / "home" / ".config",
                         .mountinfo = dir.path / "mountinfo"};

    Home() {
        fs::create_directories(sources.home / "Pictures");
        fs::create_directories(sources.home / "Downloads");
        fs::create_directories(sources.config_home);
        std::ofstream{sources.mountinfo} << "";
    }
};

}  // namespace

TEST_CASE("places: user-dirs.dirs") {
    const fs::path home{"/home/me"};
    const std::vector<UserDir> dirs = rummage::fs::parse_user_dirs(
        "# written by xdg-user-dirs-update\n"
        "XDG_DESKTOP_DIR=\"$HOME/Scrivania\"\n"
        "XDG_DOWNLOAD_DIR=\"$HOME/Scaricati\"\n"
        "XDG_TEMPLATES_DIR=\"$HOME/\"\n"
        "XDG_PICTURES_DIR=\"/data/photos\"\n"
        "XDG_MUSIC_DIR=\"relative/not/allowed\"\n"
        "  XDG_VIDEOS_DIR = \"$HOME/Video\"  \n"
        "garbage\n",
        home);
    CHECK(dirs == std::vector<UserDir>{{.key = "DESKTOP", .path = "/home/me/Scrivania"},
                                       {.key = "DOWNLOAD", .path = "/home/me/Scaricati"},
                                       {.key = "PICTURES", .path = "/data/photos"},
                                       {.key = "VIDEOS", .path = "/home/me/Video"}});
}

TEST_CASE("places: GTK bookmarks") {
    const std::vector<Place> places = rummage::fs::parse_bookmarks(
        "file:///run/media/me/DATA/fotografia Photos\n"
        "file:///tmp/with%20space\n"
        "sftp://host/home/me Remote\n"
        "file:///bad%zzescape\n"
        "\n");
    REQUIRE(places.size() == 4);
    CHECK(places[0].label == "Photos");
    CHECK(places[0].path == "/run/media/me/DATA/fotografia");
    CHECK(places[0].section == Place::Section::bookmarks);
    CHECK(places[0].uri.empty());
    CHECK(places[1].label == "with space");
    CHECK(places[1].path == "/tmp/with space");
    // A remote one has no path until gvfs mounts it.
    CHECK(places[2].label == "Remote");
    CHECK(places[2].uri == "sftp://host/home/me");
    CHECK(places[2].path.empty());
    CHECK_FALSE(places[2].disk.mounted);
    CHECK(places[3].path == "/bad%zzescape");
}

TEST_CASE("bookmarks: added at the end as GLib writes them, removed, the rest kept") {
    const std::string text =
        "sftp://host/home/me Remote\n"
        "file:///run/media/me/DATA/fotografia Photos\n"
        "file:///tmp/with%20space";  // no newline at the end
    const std::optional<std::string> added =
        rummage::fs::add_bookmark(text, "/home/me/Viaggi/naïve 2026/");
    REQUIRE(added);
    CHECK(added.value_or("") == text + "\nfile:///home/me/Viaggi/na%C3%AFve%202026\n");
    // Already there, with or without the slash, as written or decoded.
    CHECK_FALSE(rummage::fs::add_bookmark(text, "/tmp/with space"));
    CHECK_FALSE(rummage::fs::add_bookmark(text, "/run/media/me/DATA/fotografia/"));
    CHECK(rummage::fs::parse_bookmarks(added.value_or("")).back().path ==
          "/home/me/Viaggi/naïve 2026");

    const std::optional<std::string> removed =
        rummage::fs::remove_bookmark(added.value_or(""), "/run/media/me/DATA/fotografia");
    CHECK(removed.value_or("") ==
          "sftp://host/home/me Remote\n"
          "file:///tmp/with%20space\n"
          "file:///home/me/Viaggi/na%C3%AFve%202026\n");
    CHECK_FALSE(rummage::fs::remove_bookmark(text, "/elsewhere"));
    CHECK(rummage::fs::add_bookmark("", "/a") == std::optional<std::string>{"file:///a\n"});
}

TEST_CASE("bookmarks: renamed, the URI as written and the other lines kept") {
    const std::string text =
        "sftp://host/home/me Remote\n"
        "file:///run/media/me/DATA/fotografia Photos\n"
        "file:///tmp/with%20space";  // no newline at the end
    CHECK(rummage::fs::rename_bookmark(text, "/tmp/with space/", "  Two words, naïve ") ==
          std::optional<std::string>{"sftp://host/home/me Remote\n"
                                     "file:///run/media/me/DATA/fotografia Photos\n"
                                     "file:///tmp/with%20space Two words, naïve"});
    // Empty, or the folder's own name: no label, the sidebar shows the name.
    const std::string plain =
        "sftp://host/home/me Remote\n"
        "file:///run/media/me/DATA/fotografia\n"
        "file:///tmp/with%20space";
    CHECK(rummage::fs::rename_bookmark(text, "/run/media/me/DATA/fotografia", "") ==
          std::optional<std::string>{plain});
    CHECK(rummage::fs::rename_bookmark(text, "/run/media/me/DATA/fotografia", "fotografia") ==
          std::optional<std::string>{plain});
    // One line, whatever was typed.
    CHECK(rummage::fs::rename_bookmark("file:///a\n", "/a", "x\ny\tz") ==
          std::optional<std::string>{"file:///a x y z\n"});
    CHECK(rummage::fs::parse_bookmarks(
              rummage::fs::rename_bookmark(text, "/tmp/with space", "Spaced").value_or(""))
              .back()
              .label == "Spaced");
    CHECK_FALSE(rummage::fs::rename_bookmark(text, "/elsewhere", "x"));
}

TEST_CASE("bookmarks: pointed elsewhere, keeping the label and the place") {
    const std::string text = "file:///a Alpha\nfile:///b\nfile:///c\n";
    CHECK(rummage::fs::repoint_bookmark(text, "/a", "/home/me/naïve 2026/") ==
          std::optional<std::string>{
              "file:///home/me/na%C3%AFve%202026 Alpha\nfile:///b\nfile:///c\n"});
    CHECK(rummage::fs::repoint_bookmark(text, "/b/", "/z") ==
          std::optional<std::string>{"file:///a Alpha\nfile:///z\nfile:///c\n"});
    CHECK_FALSE(rummage::fs::repoint_bookmark(text, "/elsewhere", "/z"));
}

TEST_CASE("bookmarks: moved among the shown ones, the others staying put") {
    const std::string text =
        "file:///a\n"
        "sftp://host/x Remote\n"
        "file:///b Bee\n"
        "\n"
        "file:///c";
    CHECK(rummage::fs::move_bookmark(text, "/b", -1) ==
          std::optional<std::string>{"file:///b Bee\n"
                                     "sftp://host/x Remote\n"
                                     "file:///a\n"
                                     "\n"
                                     "file:///c"});
    CHECK(rummage::fs::move_bookmark(text, "/b", 1) ==
          std::optional<std::string>{"file:///a\n"
                                     "sftp://host/x Remote\n"
                                     "file:///c\n"
                                     "\n"
                                     "file:///b Bee"});
    CHECK_FALSE(rummage::fs::move_bookmark(text, "/a", -1));  // first already
    CHECK_FALSE(rummage::fs::move_bookmark(text, "/c", 1));   // last already
    CHECK_FALSE(rummage::fs::move_bookmark(text, "/elsewhere", 1));
    CHECK_FALSE(rummage::fs::move_bookmark(text, "/a", 0));
}

TEST_CASE("bookmarks: written all at once, the folder made, the permissions kept") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "config" / "gtk-3.0" / "bookmarks";
    CHECK_FALSE(rummage::core::write_file_atomically(file, "file:///a\n"));
    CHECK(rummage::core::read_text_file(file).text == "file:///a\n");
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
           std::filesystem::perms::group_read | std::filesystem::perms::others_read));
    std::filesystem::permissions(
        file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    CHECK_FALSE(rummage::core::write_file_atomically(file, "file:///b\n"));
    CHECK(rummage::core::read_text_file(file).text == "file:///b\n");
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    // Nothing left beside it.
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator{file.parent_path()}) {
        ++files;
    }
    CHECK(files == 1);
    // A folder where the file should be: an error, nothing else.
    CHECK(rummage::core::write_file_atomically(dir.path / "config", "x"));
}

TEST_CASE("places: mounts") {
    const std::vector<Place> places = rummage::fs::parse_mounts(
        "22 1 0:21 / / rw,relatime - btrfs /dev/nvme0n1p2 rw\n"
        "38 27 8:0 / /run/media/me/DATA rw,noatime - ext4 /dev/sda rw\n"
        "39 27 8:17 / /run/media/me/MY\\040STICK rw - vfat /dev/sdb1 rw\n"
        "40 27 8:18 / /mnt/backup rw - ext4 /dev/sdc1 rw\n"
        "41 27 8:18 / /mnt/backup rw - ext4 /dev/sdc1 rw\n"
        "42 27 0:5 / /media rw - tmpfs tmpfs rw\n"
        "43 27 0:6 / /home/me/mnt/x rw - tmpfs tmpfs rw\n"
        "short line\n");
    CHECK(labels(places) == std::vector<std::string>{"DATA", "MY STICK", "backup"});
    CHECK(places[1].path == "/run/media/me/MY STICK");
    CHECK(places[0].section == Place::Section::devices);
}

TEST_CASE("places: load, without user-dirs.dirs") {
    Home home;
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv Served\n";
    std::ofstream{home.sources.mountinfo}
        << "38 27 8:0 / /run/media/me/DATA rw - ext4 /dev/sda rw\n";
    const std::vector<Place> places = rummage::fs::load_places(home.sources);
    // The usual names that exist, in the sidebar's order.
    CHECK(labels(places) == std::vector<std::string>{"Home", "Downloads", "Pictures", "File system",
                                                     "Trash", "Served", "DATA"});
    CHECK(places[0].path == home.sources.home);
    CHECK(places[3].path == "/");
    CHECK(places[4].path == rummage::fs::trash_path());
}

TEST_CASE("places: load, with user-dirs.dirs") {
    Home home;
    fs::create_directories(home.sources.home / "Immagini");
    std::ofstream{home.sources.config_home / "user-dirs.dirs"}
        << "XDG_PICTURES_DIR=\"$HOME/Immagini\"\nXDG_MUSIC_DIR=\"$HOME/Musica\"\n";
    // Downloads exists but the file does not name it; Musica is named but missing.
    CHECK(labels(rummage::fs::load_places(home.sources)) ==
          std::vector<std::string>{"Home", "Immagini", "File system", "Trash"});
}

TEST_CASE("places: the watcher follows the bookmarks") {
    Home home;
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    EventLoop loop;
    int changes = 0;
    rummage::fs::PlacesWatcher watcher{loop, home.sources, [&] { ++changes; }};
    CHECK(watcher.places().size() == 5);
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv\n";
    CHECK(run_until(loop, [&] { return changes > 0; }));
    CHECK(labels(watcher.places()).back() == "srv");
}

TEST_CASE("places: icons exist in the icon font") {
    rummage::ui::FontSystem fonts;
    rummage::ui::IconSet icons{fonts};
    REQUIRE(icons.available());
    Home home;
    std::ofstream{home.sources.mountinfo} << "38 27 8:0 / /mnt/x rw - ext4 /dev/sda rw\n";
    std::ofstream{home.sources.config_home / "user-dirs.dirs"}
        << "XDG_DESKTOP_DIR=\"$HOME/a\"\nXDG_DOCUMENTS_DIR=\"$HOME/b\"\n"
           "XDG_DOWNLOAD_DIR=\"$HOME/c\"\nXDG_MUSIC_DIR=\"$HOME/d\"\n"
           "XDG_PICTURES_DIR=\"$HOME/e\"\nXDG_VIDEOS_DIR=\"$HOME/f\"\n";
    for (const char* name : {"a", "b", "c", "d", "e", "f"}) {
        fs::create_directories(home.sources.home / name);
    }
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv\n";
    const std::vector<Place> places = rummage::fs::load_places(home.sources);
    CHECK(places.size() == 11);
    // User directories above have arbitrary names; customization follows their XDG purpose.
    std::vector<std::string_view> keys;
    for (const auto& place : places) {
        keys.push_back(place.icon_key);
    }
    CHECK(keys == std::vector<std::string_view>{"home", "desktop", "documents", "downloads",
                                                "music", "pictures", "videos", "filesystem",
                                                "trash", "", ""});
    for (const Place& place : places) {
        CHECK_MESSAGE(icons.has(place.icon), place.icon);
    }
    CHECK(icons.has("trash-filled"));  // the trash's when it is not empty
}

TEST_CASE("places: the place holding a folder") {
    const std::vector<Place> places{
        {.label = "Home", .path = "/home/me", .icon = "folder"},
        {.label = "Pictures", .path = "/home/me/Pictures", .icon = "folder"},
        {.label = "File system", .path = "/", .icon = "folder"},
        {.label = "Pic", .path = "/home/me/Pic", .icon = "folder"},
    };
    CHECK(place_holding(places, "/home/me/Pictures/2026") == 1U);
    CHECK(place_holding(places, "/home/me/Pictures/") == 1U);
    CHECK(place_holding(places, "/home/me/Pictures2") == 0U);  // whole names only
    CHECK(place_holding(places, "/home/me") == 0U);
    CHECK(place_holding(places, "/etc") == 2U);
    CHECK_FALSE(place_holding(std::vector<Place>{places[1]}, "/etc").has_value());
}

TEST_CASE(
    "places: bookmark folder icons resolve asynchronously and refresh without stale results") {
    TempDir dir;
    dir.write(".directory", "[Desktop Entry]\nIcon=folder-development\n");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape) {}};
    const auto bookmarks = [&] {
        return rummage::fs::parse_bookmarks("file://" + dir.path.string() +
                                            " Projects\nsftp://host/share Remote\n");
    };
    browser.set_places(bookmarks());
    REQUIRE(run_until(
        loop, [&] { return browser.sidebar().places()[0].folder_icon == "folder-development"; }));
    CHECK(browser.sidebar().places()[0].icon == "bookmark");
    CHECK(browser.sidebar().places()[1].folder_icon.empty());
    CHECK(browser.sidebar().places()[1].icon == "server");

    for (const auto& path : {std::string{"./custom.svg"}, (dir.path / "custom.svg").string()}) {
        dir.write(".directory", "[Desktop Entry]\nIcon=folder-music\n");
        browser.set_places(browser.sidebar().places());
        REQUIRE(run_until(
            loop, [&] { return browser.sidebar().places()[0].folder_icon == "folder-music"; }));
        // Untrusted folder metadata cannot turn a bookmark icon into an image-file load.
        dir.write(".directory", "[Desktop Entry]\nIcon=" + path + "\n");
        browser.set_places(browser.sidebar().places());
        REQUIRE(run_until(loop, [&] { return browser.sidebar().places()[0].folder_icon.empty(); }));
    }
    dir.write(".directory", "[Desktop Entry]\nIcon=\n");
    browser.set_places(browser.sidebar().places());
    REQUIRE(run_until(loop, [&] { return browser.sidebar().places()[0].folder_icon.empty(); }));

    // An old completion must not restore a removed bookmark.
    browser.set_places(bookmarks());
    browser.set_places({});
    bool drained = false;
    pool.submit(
        rummage::core::Priority::prefetch, [](const rummage::core::CancelToken&) { return true; },
        [&](bool result) { drained = result; });
    REQUIRE(run_until(loop, [&] { return drained; }));
    CHECK(browser.sidebar().places().empty());
}

TEST_CASE("places: the sidebar opens a place from the keyboard") {
    TempDir dir;
    fs::create_directories(dir.path / "a" / "inner");
    fs::create_directories(dir.path / "b");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_places({{.label = "a", .path = dir.path / "a", .icon = "folder"},
                        {.label = "b", .path = dir.path / "b", .icon = "folder"}});
    browser.open(dir.path / "a");
    browser.set_mode(Browser::Mode::grid);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));

    const auto now = Browser::Clock::now();
    browser.key(key(XKB_KEY_F6), now);
    CHECK(browser.sidebar().focused());
    const float grid_size = browser.grid().icon_size();
    REQUIRE(browser.key(key(XKB_KEY_plus, true), now));
    CHECK(browser.sidebar().list_size() == 2);
    CHECK(browser.grid().icon_size() == grid_size);
    CHECK(browser.details().list_size() == 1);
    CHECK(browser.state().sidebar_size == 2);
    REQUIRE(browser.key(key(XKB_KEY_0, true), now));
    CHECK(browser.sidebar().list_size() == 1);
    // Without a draw the sidebar does not know the current place yet: it starts on the first.
    CHECK(browser.sidebar().cursor() == 0U);
    browser.key(key(XKB_KEY_Down), now);
    CHECK(browser.sidebar().cursor() == 1U);
    CHECK_FALSE(browser.grid().folder()->cursor().has_value());  // the view did not move
    browser.key(key(XKB_KEY_Return), now);
    CHECK_FALSE(browser.sidebar().focused());
    CHECK(browser.mode() == Browser::Mode::grid);
    CHECK(browser.grid().folder()->path() == dir.path / "b");

    browser.key(key(XKB_KEY_F6), now);
    browser.key(key(XKB_KEY_Escape), now);
    CHECK_FALSE(browser.sidebar().focused());
    CHECK(browser.grid().folder()->path() == dir.path / "b");

    // Ctrl+B hides it and takes the keyboard back; F6 shows it again.
    browser.key(key(XKB_KEY_F6), now);
    browser.key(key(XKB_KEY_b, true), now);
    CHECK_FALSE(browser.sidebar_visible());
    CHECK_FALSE(browser.sidebar().focused());
    browser.key(key(XKB_KEY_F6), now);
    CHECK(browser.sidebar_visible());
    CHECK(browser.sidebar().focused());
}

TEST_CASE("places: sidebar zoom handles fractional wheels, limits and reset without places") {
    rummage::views::Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {}, [](const Place&) {}};
    const auto now = Browser::Clock::now();
    rummage::ui::PointerEvent wheel;
    wheel.kind = rummage::ui::PointerEvent::Kind::scroll;
    wheel.modifiers.ctrl = true;
    wheel.wheel_y = -0.5F;
    sidebar.pointer(wheel, now);
    CHECK(sidebar.list_size() == 1);
    sidebar.pointer(wheel, now);
    CHECK(sidebar.list_size() == 2);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(sidebar.key(key(XKB_KEY_KP_Add, true)));
    }
    CHECK(sidebar.list_size() == 4);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(sidebar.key(key(XKB_KEY_minus, true)));
    }
    CHECK(sidebar.list_size() == 0);
    REQUIRE(sidebar.key(key(XKB_KEY_KP_0, true)));
    CHECK(sidebar.list_size() == 1);
    CHECK_FALSE(sidebar.key(key(XKB_KEY_plus)));
    wheel.modifiers.ctrl = false;
    wheel.wheel_y = -1.0F;
    sidebar.pointer(wheel, now);
    CHECK(sidebar.list_size() == 1);
}

TEST_CASE("sidebar: organization survives missing devices and does not alter source bookmarks") {
    rummage::views::Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {}, [](const Place&) {}};
    const Place home{.section = Place::Section::places,
                     .label = "Home",
                     .path = "/home/test",
                     .icon = "home",
                     .icon_key = "home"};
    const Place docs{.section = Place::Section::places,
                     .label = "Documents",
                     .path = "/home/test/Documents",
                     .icon = "folder",
                     .icon_key = "documents"};
    const Place disk{.section = Place::Section::devices,
                     .label = "Disk",
                     .path = "/mnt/disk",
                     .icon = "device-sd-card",
                     .disk = {.volume = "stable-volume"}};
    sidebar.set_places({home, docs, disk});
    sidebar.move_place(docs, -1);
    CHECK(sidebar.places().front().path == docs.path);
    sidebar.add_separator(home);
    REQUIRE(sidebar.places().size() == 4);
    const auto divider = sidebar.places()[1];
    REQUIRE(sidebar.separator(divider));
    sidebar.move_place(divider, 1);
    CHECK(sidebar.separator(sidebar.places()[2]));
    sidebar.hide_place(disk);
    sidebar.set_places({home, docs});
    auto remounted = disk;
    remounted.path = "/media/new-mount";
    sidebar.set_places({home, docs, remounted});
    CHECK(sidebar.places().size() == 3);
    CHECK(sidebar.sources().size() == 3);
    sidebar.toggle_section(Place::Section::places);
    CHECK(sidebar.places().empty());
    rummage::views::State state;
    state.sidebar_order = sidebar.order();
    state.sidebar_hidden = sidebar.hidden();
    state.sidebar_collapsed = sidebar.collapsed();
    const auto parsed =
        rummage::views::parse_state(rummage::views::format_state(state), "sidebar.toml");
    REQUIRE(parsed.problems.empty());
    CHECK(parsed.state == state);
    sidebar.restore_places();
    CHECK(sidebar.places().size() == 4);
    sidebar.remove_separator(divider);
    CHECK(sidebar.places().size() == 3);
    CHECK(labels(sidebar.sources()) == std::vector<std::string>{"Home", "Documents", "Disk"});
}

TEST_CASE("sidebar: tree loads only expanded folders and watches changes without following links") {
    TempDir dir;
    fs::create_directories(dir.path / "branch" / "nested");
    fs::create_directories(dir.path / ".hidden");
    fs::create_directory_symlink(dir.path, dir.path / "loop");
    std::ofstream{dir.path / "file.txt"} << "file";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {}, [](const Place&) {}};
    sidebar.enable_tree(loop, pool, watcher, rummage::fs::MimeDatabase::load());
    const auto has = [&](const fs::path& path) {
        return std::ranges::any_of(sidebar.places(),
                                   [&](const Place& p) { return p.path == path; });
    };
    sidebar.set_places({{.label = "Root", .path = dir.path, .icon = "folder"}});
    CHECK_FALSE(has(dir.path / "branch"));
    sidebar.toggle_folder(dir.path);
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch"); }, 30s));
    CHECK_FALSE(has(dir.path / "branch" / "nested"));
    CHECK_FALSE(has(dir.path / "file.txt"));
    CHECK_FALSE(has(dir.path / ".hidden"));
    REQUIRE(has(dir.path / "loop"));
    sidebar.toggle_folder(dir.path / "loop");
    CHECK_FALSE(has(dir.path / "loop" / "branch"));
    sidebar.toggle_folder(dir.path / "branch");
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch" / "nested"); }));
    fs::create_directory(dir.path / "branch" / "created");
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch" / "created"); }));
    fs::remove(dir.path / "branch" / "created");
    REQUIRE(run_until(loop, [&] { return !has(dir.path / "branch" / "created"); }));
    sidebar.set_show_hidden(true);
    REQUIRE(run_until(loop, [&] { return has(dir.path / ".hidden"); }));
    sidebar.set_focused(true);
    sidebar.set_cursor_to(dir.path / "branch");
    CHECK(sidebar.key(key(XKB_KEY_Left)));
    CHECK_FALSE(has(dir.path / "branch" / "nested"));
    CHECK(sidebar.key(key(XKB_KEY_Right)));
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch" / "nested"); }));
    fs::remove_all(dir.path / "branch");
    REQUIRE(run_until(loop, [&] { return !has(dir.path / "branch"); }));
    fs::create_directories(dir.path / "branch" / "replacement");
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch"); }));
    sidebar.toggle_folder(dir.path / "branch");
    REQUIRE(run_until(loop, [&] { return has(dir.path / "branch" / "replacement"); }));
    CHECK_FALSE(has(dir.path / "branch" / "nested"));
    sidebar.set_places({});
    CHECK(sidebar.places().empty());
}

TEST_CASE("sidebar: separators drag within their section and headers collapse without opening") {
    rummage::ui::Offscreen gl{220, 350};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    std::size_t opened = 0;
    rummage::views::Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {},
                                    [&](const Place&) { ++opened; }};
    const Place a{.label = "A", .path = "/a", .icon = "folder"};
    const Place b{.label = "B", .path = "/b", .icon = "folder"};
    const Place c{.label = "C", .path = "/c", .icon = "folder"};
    sidebar.set_places({a, b, c});
    sidebar.add_separator(b);
    const auto separator = sidebar.places()[1];
    const auto draw = [&] {
        renderer.begin_frame(220, 350, 1, {});
        sidebar.draw(renderer, {0, 0, 220, 350}, {}, icons, {},
                     rummage::views::Sidebar::Clock::now());
        renderer.end_frame();
    };
    draw();
    const auto y_of = [&](const Place& target) {
        for (float y = 38; y < 350; y += 1) {
            const auto* p = sidebar.place_under(50, y);
            if (p && sidebar.place_key(*p) == sidebar.place_key(target))
                return y + 5;
        }
        return -1.0F;
    };
    const auto from = y_of(separator), to = y_of(c);
    REQUIRE(from > 0);
    REQUIRE(to > 0);
    using Event = rummage::ui::PointerEvent;
    const auto pointer = [&](Event::Kind kind, float y) {
        sidebar.pointer({.kind = kind, .x = 50, .y = y, .button = rummage::ui::PointerButton::left},
                        rummage::views::Sidebar::Clock::now());
    };
    pointer(Event::Kind::press, from);
    pointer(Event::Kind::motion, to);
    draw();
    pointer(Event::Kind::release, to);
    CHECK(labels(sidebar.places()) == std::vector<std::string>{"A", "B", "Separator", "C"});
    CHECK(opened == 0);
    draw();
    pointer(Event::Kind::press, 20);
    CHECK(sidebar.places().empty());
    CHECK(opened == 0);
    draw();
    pointer(Event::Kind::press, 20);
    CHECK(sidebar.places().size() == 4);
}

TEST_CASE("sidebar: places and bookmarks expand independently and children stay ordinary folders") {
    TempDir dir;
    fs::create_directories(dir.path / ".branch" / "nested");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape) {}};
    auto& sidebar = browser.sidebar();
    const Place home{.label = "Home", .path = dir.path, .icon = "home", .icon_key = "home"};
    const Place bookmark{.section = Place::Section::bookmarks,
                         .label = "Branch",
                         .path = dir.path / ".branch",
                         .icon = "bookmark"};
    browser.set_places({home, bookmark});
    browser.open(dir.path);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    const auto count = [&](const fs::path& path) {
        return std::ranges::count(sidebar.places(), path, &Place::path);
    };
    sidebar.set_show_hidden(true);
    sidebar.toggle_folder(home.path);
    REQUIRE(run_until(loop, [&] { return count(bookmark.path) == 2; }));
    sidebar.toggle_folder(bookmark.path, sidebar.place_key(home));
    REQUIRE(run_until(loop, [&] { return count(bookmark.path / "nested") == 1; }));
    sidebar.toggle_folder(bookmark.path, sidebar.place_key(bookmark));
    REQUIRE(run_until(loop, [&] { return count(bookmark.path / "nested") == 2; }));
    sidebar.set_show_hidden(false);
    CHECK(count(bookmark.path / "nested") == 1);
    sidebar.set_show_hidden(true);
    CHECK(count(bookmark.path / "nested") == 1);
    sidebar.toggle_folder(home.path);
    CHECK(count(bookmark.path / "nested") == 1);
    // Collapsing one root keeps the other root's watcher and expansion alive.
    fs::create_directory(bookmark.path / "created");
    REQUIRE(run_until(loop, [&] { return count(bookmark.path / "created") == 1; }));
    sidebar.set_focused(true);
    sidebar.set_cursor_to(bookmark.path / "nested");
    const auto child = *sidebar.cursor_place();
    REQUIRE(sidebar.child(child));
    CHECK(sidebar.edit_key(key(XKB_KEY_F2)));
    CHECK_FALSE(sidebar.renaming());
    sidebar.hide_place(child);
    sidebar.add_separator(child);
    CHECK(sidebar.hidden().empty());
    CHECK(sidebar.order().empty());
    browser.key(key(XKB_KEY_Menu), Browser::Clock::now());
    REQUIRE(browser.context_menu().is_open());
    const auto& items = browser.context_menu().items();
    const auto has = [&](std::string_view id) {
        return std::ranges::find(items, id, &rummage::views::MenuItem::id) != items.end();
    };
    CHECK(has("open"));
    CHECK(has("properties"));
    CHECK_FALSE(has("rename_bookmark"));
    CHECK_FALSE(has("remove_bookmark"));
    CHECK_FALSE(has("sidebar_hide"));
    browser.context_menu().close();
    browser.key(key(XKB_KEY_Return), Browser::Clock::now());
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    CHECK(browser.picker_folder()->path() == bookmark.path / "nested");
    CHECK(sidebar.sources().size() == 2);
}

TEST_CASE("sidebar: disclosures follow visible subfolders and update when a leaf gains children") {
    TempDir dir;
    fs::create_directories(dir.path / "empty");
    fs::create_directories(dir.path / "files");
    fs::create_directories(dir.path / "hidden" / ".child");
    fs::create_directories(dir.path / "branch" / "leaf");
    std::ofstream{dir.path / "files" / "file.txt"} << "file";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::size_t opened = 0;
    rummage::views::Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {},
                                    [&](const Place&) { ++opened; }};
    sidebar.enable_tree(loop, pool, watcher, rummage::fs::MimeDatabase::load());
    sidebar.set_places({{.label = "Empty", .path = dir.path / "empty", .icon = "folder"},
                        {.label = "Files", .path = dir.path / "files", .icon = "folder"},
                        {.label = "Hidden", .path = dir.path / "hidden", .icon = "folder"},
                        {.label = "Branch", .path = dir.path / "branch", .icon = "folder"}});
    rummage::ui::Offscreen gl{220, 350};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(220, 350, 1, {});
        sidebar.draw(renderer, {0, 0, 220, 350}, {}, icons, {}, Browser::Clock::now());
        renderer.end_frame();
    };
    draw();
    REQUIRE(run_until(loop, [&] { return !sidebar.loading(); }));
    CHECK_FALSE(sidebar.expandable(0));
    CHECK_FALSE(sidebar.expandable(1));
    CHECK_FALSE(sidebar.expandable(2));
    CHECK(sidebar.expandable(3));
    sidebar.set_show_hidden(true);
    CHECK(sidebar.expandable(2));
    sidebar.set_show_hidden(false);
    draw();
    const auto click = [&](std::size_t row) {
        const auto rect = sidebar.place_icon_rect(sidebar.places()[row].path);
        REQUIRE(rect);
        sidebar.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                         .x = 12,
                         .y = rect->y + 12,
                         .button = rummage::ui::PointerButton::left},
                        Browser::Clock::now());
    };
    click(0);  // No invisible expansion target on a leaf: the click opens it normally.
    CHECK(opened == 1);
    click(3);
    CHECK(opened == 1);
    REQUIRE(run_until(loop, [&] { return sidebar.places().size() == 5; }));
    draw();
    REQUIRE(run_until(loop, [&] { return !sidebar.loading(); }));
    CHECK_FALSE(sidebar.expandable(4));
    fs::create_directory(dir.path / "branch" / "leaf" / "new");
    REQUIRE(run_until(loop, [&] { return sidebar.expandable(4); }));
    fs::remove(dir.path / "branch" / "leaf" / "new");
    REQUIRE(run_until(loop, [&] { return !sidebar.expandable(4); }));
}

TEST_CASE("sidebar: logical groups expand recursively with stable URI keys and persisted state") {
    using Sidebar = rummage::views::Sidebar;
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::string opened;
    std::vector<std::string> expanded;
    Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {},
                    [&](const Place& place) { opened = place.uri; }};
    sidebar.enable_tree(loop, pool, watcher, rummage::fs::MimeDatabase::load());
    const Place root{
        .label = "Photo Library", .path = {}, .icon = "photo", .uri = "rummage:photos"};
    const Place albums{
        .label = "Albums", .path = {}, .icon = "album", .uri = "rummage:photos/albums"};
    const Place sources{
        .label = "Sources", .path = {}, .icon = "folders", .uri = "rummage:photos/sources"};
    const Place searches{
        .label = "Saved searches", .path = {}, .icon = "search", .uri = "rummage:photos/searches"};
    const Place first{
        .label = "Same name", .path = {}, .icon = "album", .uri = "rummage:photos/albums/10"};
    const Place second{
        .label = "Same name", .path = {}, .icon = "album", .uri = "rummage:photos/albums/11"};
    Sidebar::LogicalGroups groups{{root.uri, {{albums, sources, searches}, true}},
                                  {albums.uri, {{first, second}, false}},
                                  {sources.uri, {{}, false}},
                                  {searches.uri, {{}, false}}};
    sidebar.set_logical_groups(groups);
    sidebar.set_logical_expand_handler(
        [&](std::string uri) { expanded.push_back(std::move(uri)); });
    sidebar.set_places({root});
    CHECK(labels(sidebar.places()) ==
          std::vector<std::string>{"Photo Library", "Albums", "Sources", "Saved searches"});
    CHECK_FALSE(sidebar.loading());
    CHECK(sidebar.expandable(0));
    CHECK(sidebar.expandable(2));  // An empty logical group still offers lazy expansion.
    sidebar.set_focused(true);
    CHECK(sidebar.key(key(XKB_KEY_Right)));  // Expanded root -> Albums.
    REQUIRE(sidebar.cursor_place());
    CHECK(sidebar.cursor_place()->uri == albums.uri);
    CHECK(sidebar.key(key(XKB_KEY_Right)));  // Expand Albums, do not navigate.
    CHECK(opened.empty());
    CHECK(expanded == std::vector<std::string>{albums.uri});
    REQUIRE(sidebar.places().size() == 6);
    CHECK(Sidebar::child(sidebar.places()[2]));
    CHECK(Sidebar::place_key(sidebar.places()[2]) != Sidebar::place_key(sidebar.places()[3]));
    CHECK_FALSE(sidebar.expandable(2));
    CHECK(sidebar.key(key(XKB_KEY_Right)));  // First album.
    CHECK(sidebar.key(key(XKB_KEY_Return)));
    CHECK(opened == first.uri);
    sidebar.set_focused(true);
    sidebar.key(key(XKB_KEY_Home));
    sidebar.key(key(XKB_KEY_Right));
    sidebar.key(key(XKB_KEY_Left));  // Collapse Albums.
    sidebar.key(key(XKB_KEY_Left));  // Parent Photo Library.
    sidebar.key(key(XKB_KEY_Left));  // Collapse root.
    CHECK(sidebar.places().size() == 1);
    const auto persisted = sidebar.collapsed();
    Sidebar restored{[] {}, [](rummage::ui::CursorShape) {}, [](const Place&) {}};
    restored.set_organization({}, {}, persisted);
    restored.set_logical_groups(groups);
    restored.set_places({root});
    CHECK(restored.places().size() == 1);
    sidebar.key(key(XKB_KEY_Right));  // Root.
    CHECK(sidebar.places().size() == 4);
    sidebar.key(key(XKB_KEY_Right));  // Albums remains collapsed.
    sidebar.key(key(XKB_KEY_Down));   // Sources.
    sidebar.key(key(XKB_KEY_Right));  // Empty group requests children.
    CHECK(expanded.back() == sources.uri);
    CHECK_FALSE(sidebar.loading());
    groups.at(sources.uri)
        .children.push_back(
            {.label = "Pictures", .path = {}, .icon = "photo", .uri = "rummage:photos/sources/8"});
    sidebar.set_logical_groups(groups);
    REQUIRE(sidebar.cursor_place());
    CHECK(sidebar.cursor_place()->uri == sources.uri);
    CHECK(sidebar.places().size() == 5);
    for (const auto& place : sidebar.places())
        CHECK(place.path.empty());
    CHECK_FALSE(sidebar.loading());
    sidebar.toggle_folder({});  // Empty paths never alias logical rows.
    CHECK(sidebar.places().size() == 5);
}

TEST_CASE("sidebar: collapsed logical groups highlight the closest visible ancestor") {
    using Sidebar = rummage::views::Sidebar;
    rummage::ui::Offscreen gl{260, 420};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    Sidebar sidebar{[] {}, [](rummage::ui::CursorShape) {}, [](const Place&) {}};
    sidebar.enable_tree(loop, pool, watcher, rummage::fs::MimeDatabase::load());
    const Place root{
        .label = "Photo Library", .path = {}, .icon = "photo", .uri = "rummage:photos"};
    const Place albums{
        .label = "Albums", .path = {}, .icon = "album", .uri = "rummage:photos/albums"};
    const Place album{
        .label = "Summer", .path = {}, .icon = "album", .uri = "rummage:photos/albums/3"};
    sidebar.set_logical_groups({{root.uri, {{albums}, true}}, {albums.uri, {{album}, false}}});
    sidebar.set_places({root});
    sidebar.set_current_uri(album.uri);
    const auto draw = [&] {
        renderer.begin_frame(260, 420, 1, {});
        sidebar.draw(renderer, {0, 0, 260, 420}, {}, icons, {}, Sidebar::Clock::now());
        renderer.end_frame();
    };
    draw();
    REQUIRE(sidebar.current_place());
    CHECK(sidebar.current_place()->uri == albums.uri);
    CHECK(sidebar.current_uri() == album.uri);
    REQUIRE(sidebar.place_icon_rect_for_uri(root.uri));
    REQUIRE(sidebar.place_icon_rect_for_uri(albums.uri));
    CHECK_FALSE(sidebar.place_icon_rect_for_uri(album.uri));
    sidebar.set_focused(true);
    sidebar.key(key(XKB_KEY_Right));  // Open highlighted Albums.
    draw();
    REQUIRE(sidebar.current_place());
    CHECK(sidebar.current_place()->uri == album.uri);
    const auto parent = sidebar.place_icon_rect_for_uri(albums.uri);
    const auto leaf = sidebar.place_icon_rect_for_uri(album.uri);
    REQUIRE(parent);
    REQUIRE(leaf);
    CHECK(leaf->x > parent->x);
    CHECK_FALSE(sidebar.loading());  // Drawing logical rows never probes subfolders.
    sidebar.key(key(XKB_KEY_Home));
    sidebar.key(key(XKB_KEY_Left));
    draw();
    REQUIRE(sidebar.current_place());
    CHECK(sidebar.current_place()->uri == root.uri);
    CHECK_FALSE(sidebar.place_icon_rect_for_uri(albums.uri));
    CHECK_FALSE(sidebar.loading());
}
