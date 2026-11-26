#include "rummage/views/breadcrumb.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::Ellipsis;
using rummage::ui::FontSystem;
using rummage::views::Browser;
using rummage::views::Crumb;
using rummage::views::crumbs_of;
using rummage::views::Folder;
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

std::string cursor_name(const Folder& folder) {
    const rummage::fs::Entry* entry = folder.cursor_entry();
    return entry == nullptr ? std::string{} : entry->name;
}

// root/a/b/c
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    Fixture() { fs::create_directories(root / "a" / "b" / "c"); }

    // The folder shown has loaded and its cursor landed.
    bool settled() {
        return run_until(loop, [&] { return !browser.loading(); });
    }
};

}  // namespace

TEST_CASE("breadcrumb: steps from the home or the root") {
    CHECK(crumbs_of("/home/me/Pictures/2026", "/home/me") ==
          std::vector<Crumb>{{.path = "/home/me", .label = "", .icon = "home"},
                             {.path = "/home/me/Pictures", .label = "Pictures", .icon = ""},
                             {.path = "/home/me/Pictures/2026", .label = "2026", .icon = ""}});
    CHECK(crumbs_of("/home/me", "/home/me") ==
          std::vector<Crumb>{{.path = "/home/me", .label = "", .icon = "home"}});
    CHECK(crumbs_of("/etc/xdg/", "/home/me") ==
          std::vector<Crumb>{{.path = "/", .label = "", .icon = "server"},
                             {.path = "/etc", .label = "etc", .icon = ""},
                             {.path = "/etc/xdg", .label = "xdg", .icon = ""}});
    CHECK(crumbs_of("/", "/home/me") ==
          std::vector<Crumb>{{.path = "/", .label = "", .icon = "server"}});
    // A sibling whose name starts like the home is not in it.
    CHECK(crumbs_of("/home/me2", "/home/me").front().icon == "server");
    CHECK(crumbs_of("/home/me/x", "").front().icon == "server");
}

TEST_CASE("breadcrumb: measured names fit after logical-width rounding at fractional scales") {
    constexpr float kTextSize = 14.0F;
    constexpr float kInsets = 12.0F;
    FontSystem fonts;
    for (const float output : {1.0F, 1.25F, 1.5F, 2.0F}) {
        // Exercise non-binary scales, including 15/13, without depending on a UI size setting.
        for (int numerator = 8; numerator <= 32; ++numerator) {
            const float scale = output * (static_cast<float>(numerator) / 13.0F);
            for (const std::string_view name :
                 {"Downloads", "Documents", "Pictures", "Desktop", "Templates", "2026"}) {
                CAPTURE(scale);
                CAPTURE(name);
                const float step = (fonts.layout(name, kTextSize, scale).width / scale) + kInsets;
                const float room = step - kInsets;
                CHECK_FALSE(fonts.layout(name, kTextSize, scale, room, Ellipsis::middle).truncated);
            }
        }
    }
}

TEST_CASE("breadcrumb: width tolerance stays within one physical 26.6 unit") {
    constexpr float kTextSize = 14.0F;
    constexpr float kUnit = 1.0F / 64.0F;
    constexpr std::string_view kName = "Downloads";
    FontSystem fonts;
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        const float width = fonts.layout(kName, kTextSize, scale).width;
        for (const Ellipsis ellipsis : {Ellipsis::end, Ellipsis::middle}) {
            CHECK_FALSE(
                fonts.layout(kName, kTextSize, scale, (width - kUnit / 2.0F) / scale, ellipsis)
                    .truncated);
            CHECK(fonts.layout(kName, kTextSize, scale, (width - 2.0F * kUnit) / scale, ellipsis)
                      .truncated);
        }
    }
}

TEST_CASE("breadcrumb: going up in the grid and the details") {
    for (const Browser::Mode mode : {Browser::Mode::grid, Browser::Mode::details}) {
        Fixture f;
        f.browser.open(f.root / "a" / "b" / "c");
        f.browser.set_mode(mode);
        REQUIRE(f.settled());
        f.browser.go_to(f.root / "a", "b");
        REQUIRE(f.settled());
        const std::shared_ptr<Folder> folder =
            mode == Browser::Mode::grid ? f.browser.grid().folder() : f.browser.details().folder();
        CHECK(folder->path() == f.root / "a");
        CHECK(cursor_name(*folder) == "b");
        CHECK(f.browser.mode() == mode);
    }
}

TEST_CASE("breadcrumb: going up in the columns") {
    Fixture f;
    f.browser.reveal(f.root, f.root / "a" / "b" / "c");
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    REQUIRE(columns.column_count() >= 3);

    // A folder with a column: that column is focused, its cursor still on the way down; the
    // folder we came from shows again from the top, the columns after it close.
    f.browser.go_to(f.root / "a", "b");
    CHECK(columns.focused_folder()->path() == f.root / "a");
    CHECK(cursor_name(*columns.focused_folder()) == "b");
    REQUIRE(columns.column_count() == 3);
    CHECK(columns.column(2).folder().path() == f.root / "a" / "b");
    REQUIRE(f.settled());
    CHECK_FALSE(columns.column(2).folder().cursor().has_value());
    CHECK(columns.column_count() == 3);

    // A folder above the first column: columns from it, the cursor on where we came from.
    f.browser.open(f.root / "a" / "b");
    REQUIRE(f.settled());
    f.browser.go_to(f.root, "a");
    REQUIRE(f.settled());
    CHECK(columns.column(0).folder().path() == f.root);
    CHECK(cursor_name(columns.column(0).folder()) == "a");
}

TEST_CASE("breadcrumb: inline path supports selection, clipboard, navigation and cancellation") {
    Fixture f;
    f.browser.open(f.root);
    REQUIRE(f.settled());
    rummage::ui::Offscreen surface{1200, 800};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(1200, 800, 1, {});
        f.browser.draw(renderer, {0, 0, 1200, 800}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
    };
    auto& crumb = f.browser.breadcrumb();
    std::string copied;
    std::function<void(const std::optional<std::string>&)> deliver;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { copied = formats.front().second; };
    clipboard.types = [] { return std::vector<std::string>{"text/plain;charset=utf-8"}; };
    clipboard.read = [&](auto, auto callback) { deliver = std::move(callback); };
    f.browser.set_system_clipboard(std::move(clipboard));
    const auto press = [&](xkb_keysym_t sym, rummage::ui::Modifiers modifiers = {}) {
        f.browser.key({.keysym = sym, .text = {}, .modifiers = modifiers}, Browser::Clock::now());
    };
    const auto edit = [&] {
        draw();
        bool clicked = false;
        for (float y = 0; y < 160; y += 1) {
            if (crumb.claims(1100, y)) {
                crumb.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                               .x = 1100,
                               .y = y,
                               .button = rummage::ui::PointerButton::left});
                clicked = true;
                break;
            }
        }
        REQUIRE(clicked);
        REQUIRE(crumb.editing());
    };
    edit();
    CHECK(crumb.text() == f.root.string());
    press(XKB_KEY_c, {.ctrl = true});
    CHECK(copied == f.root.string());
    press(XKB_KEY_v, {.ctrl = true});
    REQUIRE(deliver);
    deliver((f.root / "a").string());
    press(XKB_KEY_Return);
    REQUIRE(f.settled());
    CHECK_FALSE(crumb.editing());
    CHECK(f.browser.picker_folder()->path() == f.root / "a");
    edit();
    crumb.paste((f.root / "missing").string());
    press(XKB_KEY_Return);
    REQUIRE(f.settled());
    CHECK(crumb.editing());
    CHECK_FALSE(crumb.error().empty());
    CHECK(f.browser.picker_folder()->path() == f.root / "a");
    press(XKB_KEY_Escape);
    CHECK_FALSE(crumb.editing());
    edit();
    CHECK(crumb.text() == (f.root / "a").string());
    press(XKB_KEY_v, {.ctrl = true});
    press(XKB_KEY_Escape);
    edit();
    deliver((f.root / "b").string());
    CHECK(crumb.text() == (f.root / "a").string());  // stale paste cannot enter a new edit
    f.browser.key({.keysym = XKB_KEY_slash, .text = f.root.string(), .modifiers = {}},
                  Browser::Clock::now());
    press(XKB_KEY_Return);
    REQUIRE(f.settled());
    CHECK(f.browser.picker_folder()->path() == f.root);
}

TEST_CASE("breadcrumb: completion cycles Unicode paths and ignores stale results") {
    Fixture f;
    f.browser.open(f.root);
    REQUIRE(f.settled());
    rummage::ui::Offscreen surface{1200, 800};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&] {
        renderer.begin_frame(1200, 800, 1, {});
        f.browser.draw(renderer, {0, 0, 1200, 800}, {}, icons, Browser::Clock::now());
        renderer.end_frame();
    };
    auto& crumb = f.browser.breadcrumb();
    std::string copied;
    std::function<void(const std::optional<std::string>&)> deliver;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { copied = formats.front().second; };
    clipboard.types = [] { return std::vector<std::string>{"text/plain;charset=utf-8"}; };
    clipboard.read = [&](auto, auto callback) { deliver = std::move(callback); };
    f.browser.set_system_clipboard(std::move(clipboard));
    const auto press = [&](xkb_keysym_t sym, rummage::ui::Modifiers modifiers = {}) {
        f.browser.key({.keysym = sym, .text = {}, .modifiers = modifiers}, Browser::Clock::now());
    };
    const auto edit = [&] {
        draw();
        bool clicked = false;
        for (float y = 0; y < 160; y += 1) {
            if (crumb.claims(1100, y)) {
                crumb.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                               .x = 1100,
                               .y = y,
                               .button = rummage::ui::PointerButton::left});
                clicked = true;
                break;
            }
        }
        REQUIRE(clicked);
        REQUIRE(crumb.editing());
    };
    fs::create_directory(f.root / "École");
    fs::create_directory(f.root / "Été");
    std::ofstream{f.root / "notes.txt"} << "text";
    edit();
    crumb.paste((f.root / "É").string());
    press(XKB_KEY_Tab);
    REQUIRE(run_until(f.loop, [&] { return !crumb.completing(); }));
    CHECK(crumb.text() == (f.root / "École").string() + "/");
    press(XKB_KEY_Tab);
    CHECK(crumb.text() == (f.root / "Été").string() + "/");
    press(XKB_KEY_Tab, {.shift = true});
    CHECK(crumb.text() == (f.root / "École").string() + "/");
    press(XKB_KEY_a, {.ctrl = true});
    crumb.paste((f.root / "É").string());
    press(XKB_KEY_Tab);
    press(XKB_KEY_a, {.ctrl = true});
    crumb.paste((f.root / "notes.txt").string());
    REQUIRE(f.settled());
    CHECK(crumb.text() == (f.root / "notes.txt").string());
    press(XKB_KEY_Return);
    REQUIRE(f.settled());
    CHECK_FALSE(crumb.editing());
    REQUIRE(f.browser.picker_folder()->cursor_entry());
    CHECK(f.browser.picker_folder()->cursor_entry()->name == "notes.txt");
    edit();
    crumb.paste((f.root / "a").string());
    press(XKB_KEY_Return);
    // Navigation is pending; a newer edit supersedes it before the worker returns.
    press(XKB_KEY_a, {.ctrl = true});
    crumb.paste((f.root / "Été").string());
    REQUIRE(f.settled());
    CHECK(f.browser.picker_folder()->path() == f.root);
    CHECK(crumb.editing());
    CHECK(crumb.text() == (f.root / "Été").string());
    press(XKB_KEY_Return);
    REQUIRE(f.settled());
    CHECK(f.browser.picker_folder()->path() == f.root / "Été");
}

TEST_CASE("breadcrumb: clicking the current crumb appends while keyboard editing selects all") {
    Fixture f;
    SUBCASE("browser") {}
    SUBCASE("picker") { f.browser.set_picker_mode(true); }
    f.browser.open(f.root);
    REQUIRE(f.settled());
    rummage::ui::Offscreen surface{1200, 800};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(1200, 800, 1, {});
    f.browser.draw(renderer, {0, 0, 1200, 800}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    auto& crumb = f.browser.breadcrumb();
    bool clicked = false;
    for (float y = 0; y < 160 && !clicked; y += 1) {
        for (float x = 0; x < 1200; x += 1) {
            if (!crumb.text_at(x, y))
                continue;
            CHECK(crumb.path_at(x, y) == f.root);
            crumb.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                           .x = x,
                           .y = y,
                           .button = rummage::ui::PointerButton::left});
            clicked = true;
            break;
        }
    }
    REQUIRE(clicked);
    REQUIRE(crumb.editing());
    CHECK(crumb.copy_selection(false).empty());
    crumb.paste("/a");
    CHECK(crumb.text() == (f.root / "a").string());
    crumb.cancel_edit();
    crumb.edit(f.root);
    CHECK(crumb.copy_selection(false) == f.root.string());
}

TEST_CASE("columns: reset root keeps focused folder selection and descendants") {
    Fixture f;
    f.browser.reveal(f.root, f.root / "a" / "b");
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    const auto focused = columns.focused_folder();
    REQUIRE(focused);
    const auto before = columns.column_count() - columns.focus();
    const auto name = cursor_name(*focused);
    columns.reset_root();
    CHECK(columns.focus() == 0);
    CHECK(columns.focused_folder() == focused);
    CHECK(columns.column_count() == before);
    CHECK(cursor_name(*focused) == name);
    columns.reset_root();
    CHECK(columns.focused_folder() == focused);
    for (const auto key : {XKB_KEY_BackSpace, XKB_KEY_Left}) {
        f.browser.key({.keysym = static_cast<xkb_keysym_t>(key), .text = {}, .modifiers = {}},
                      Browser::Clock::now());
        REQUIRE(f.settled());
        CHECK(columns.focused_folder() == focused);
        CHECK(columns.focus() == 0);
    }
    f.browser.open(f.root);
    REQUIRE(f.settled());
    f.browser.key({.keysym = XKB_KEY_BackSpace, .text = {}, .modifiers = {}},
                  Browser::Clock::now());
    REQUIRE(f.settled());
    CHECK(columns.focused_folder()->path() == f.root.parent_path());
}

TEST_CASE("columns: reset root inside an archive preserves selection settings and Back") {
    Fixture f;
    const auto archive = f.root / "basic.zip";
    fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "archive" / "basic.zip", archive);
    rummage::fs::forget_archive_trees();
    f.browser.open(archive);
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    columns.focused_folder()->select_name("docs");
    rummage::ui::KeyEvent right;
    right.keysym = XKB_KEY_Right;
    f.browser.key(right, Browser::Clock::now());
    REQUIRE(f.settled());
    const auto focused = columns.focused_folder();
    REQUIRE(focused);
    REQUIRE(columns.focus() > 0);
    REQUIRE(focused->is_archive());
    REQUIRE(focused->path() == archive / "docs");
    focused->select_name("b.txt");
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_list_size(2);
    const auto order = focused->order();
    const auto history_size = f.browser.history().size();
    const auto history_index = f.browser.history_index();
    columns.reset_root();
    REQUIRE(columns.column_count() == 1);
    CHECK(columns.focus() == 0);
    CHECK(columns.focused_folder() == focused);
    CHECK(focused->model().archive_location().archive == archive);
    CHECK(focused->model().archive_location().inner == "docs");
    CHECK(cursor_name(*focused) == "b.txt");
    CHECK(focused->selected_count() == 1);
    CHECK(focused->order() == order);
    CHECK(columns.list_size() == 2);
    CHECK(f.browser.history().size() == history_size);
    CHECK(f.browser.history_index() == history_index);

    // The new root remains a usable hit target even when one column is wider than the view.
    rummage::ui::Offscreen surface{180, 320};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    columns.set_info_visible(false);
    renderer.begin_frame(180, 320, 1, {});
    columns.draw(renderer, {0, 0, 180, 320}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    const auto area = columns.column(0).rows_area();
    const auto hit = columns.hit(20, area.y + 10);
    REQUIRE(hit);
    CHECK(hit->folder == focused);

    // Resetting the columns is not a navigation step. Back still returns to the archive root.
    f.browser.back();
    REQUIRE(f.settled());
    REQUIRE(columns.focused_folder());
    CHECK(columns.focused_folder()->path() == archive);
    CHECK(columns.focused_folder()->is_archive());
    CHECK(cursor_name(*columns.focused_folder()) == "docs");
}

TEST_CASE("columns: reset root survives removal while the focused folder is loading") {
    Fixture f;
    f.browser.open(f.root);
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    const auto started = std::make_shared<std::atomic<unsigned>>(0);
    std::promise<void> release;
    const auto ready = release.get_future().share();
    // Both readers wait here, so the selected child's load cannot race its removal. Destroying
    // the promise on an assertion failure also releases the waits before Fixture joins workers.
    for (int i = 0; i < 2; ++i) {
        f.pool.submit(
            rummage::core::Priority::visible,
            [started, ready](const rummage::core::CancelToken&) {
                started->fetch_add(1);
                ready.wait();
                return 0;
            },
            [](int) {});
    }
    REQUIRE(run_until(f.loop, [&] { return started->load() == 2; }));
    columns.focused_folder()->select_name("a");
    rummage::ui::KeyEvent right;
    right.keysym = XKB_KEY_Right;
    f.browser.key(right, Browser::Clock::now());
    const auto focused = columns.focused_folder();
    REQUIRE(focused);
    REQUIRE(columns.focus() == 1);
    REQUIRE(focused->path() == f.root / "a");
    REQUIRE(focused->model().loading());
    fs::remove_all(f.root / "a");
    columns.reset_root();
    CHECK(columns.focus() == 0);
    CHECK(columns.focused_folder() == focused);
    release.set_value();
    REQUIRE(
        run_until(f.loop, [&] { return focused->model().error() && !focused->model().loading(); }));
    CHECK(focused->model().error() == std::errc::no_such_file_or_directory);
    CHECK(focused->entries().empty());
    CHECK_FALSE(focused->cursor());
    CHECK(columns.column_count() == 1);
    columns.reset_root();
    CHECK(columns.focused_folder() == focused);

    // The removed root remains recoverable through normal parent navigation. Its late worker
    // completion must not reinsert it or leave an invalid focus index after the parent loads.
    rummage::ui::KeyEvent up;
    up.keysym = XKB_KEY_BackSpace;
    f.browser.key(up, Browser::Clock::now());
    REQUIRE(f.settled());
    REQUIRE(columns.focused_folder());
    CHECK(columns.focused_folder()->path() == f.root);
    CHECK_FALSE(columns.focused_folder()->model().error());
    CHECK(columns.focus() == 0);
    CHECK(columns.column_count() == 1);
    CHECK(columns.focused_folder()->entries().empty());
}

TEST_CASE("columns: vertical hierarchy keeps independent lists and hit targets") {
    Fixture f;
    f.browser.reveal(f.root, f.root / "a" / "b");
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    columns.set_info_visible(false);
    columns.set_vertical(true);
    rummage::ui::Offscreen surface{500, 900};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(500, 900, 1, {});
    columns.draw(renderer, {0, 0, 500, 900}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    REQUIRE(columns.column_count() >= 2);
    const auto a = columns.column(0).rows_area();
    const auto b = columns.column(1).rows_area();
    CHECK(a.x == b.x);
    CHECK(a.width == b.width);
    CHECK(b.y > a.y);
    const auto hit = columns.hit(b.x + 20, b.y + 10);
    REQUIRE(hit);
    CHECK(hit->folder == columns.column(1).shared_folder());
    const auto focused = columns.focused_folder();
    columns.set_vertical(false);
    CHECK(columns.focused_folder() == focused);
}

TEST_CASE("breadcrumb: native search keeps its scope clickable in the normal browser") {
    Fixture f;
    f.browser.open(f.root);
    REQUIRE(f.settled());
    f.browser.open_search_results(f.root, "type:folder", false);
    REQUIRE(f.settled());
    rummage::ui::Offscreen surface{1200, 800};
    FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(1200, 800, 1, {});
    f.browser.draw(renderer, {0, 0, 1200, 800}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    auto& crumb = f.browser.breadcrumb();
    REQUIRE_FALSE(crumb.shown().empty());
    CHECK(crumb.shown().back() == "Search Results");
    std::optional<std::pair<float, float>> scope;
    for (float y = 0; y < 160 && !scope; y += 2)
        for (float x = 0; x < 1200; x += 2)
            if (crumb.path_at(x, y) == f.root) {
                scope = {x, y};
                break;
            }
    REQUIRE(scope);
    f.browser.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                       .x = scope->first,
                       .y = scope->second,
                       .button = rummage::ui::PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = rummage::ui::PointerEvent::Kind::release,
                       .x = scope->first,
                       .y = scope->second,
                       .button = rummage::ui::PointerButton::left},
                      Browser::Clock::now());
    REQUIRE(f.settled());
    CHECK_FALSE(f.browser.searching_results());
    CHECK(f.browser.picker_folder()->path() == f.root);
    f.browser.back();
    REQUIRE(f.settled());
    CHECK(f.browser.searching_results());
    CHECK(f.browser.name_filter() == "type:folder");
}
