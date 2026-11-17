#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
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
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::DirectoryWatcher;
using rummage::fs::SortKey;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::DetailsView;
using rummage::views::Folder;
using rummage::views::InfoPane;
using Field = DetailsView::Field;
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

void write(const fs::path& file, std::size_t bytes) {
    std::ofstream{file} << std::string(bytes, 'x');
}

std::string cursor_name(const Folder& folder) {
    const rummage::fs::Entry* entry = folder.cursor_entry();
    return entry == nullptr ? std::string{} : entry->name;
}

std::vector<std::string> names(const Folder& folder) {
    std::vector<std::string> out;
    for (const rummage::fs::Entry& entry : folder.entries()) {
        out.push_back(entry.name);
    }
    return out;
}

// root/{alpha/{inner.txt}, gamma/, a.md (10 bytes), b.txt (100), c.txt (1)}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    DetailsView& details = browser.details();

    Fixture() {
        fs::create_directories(root / "alpha");
        fs::create_directories(root / "gamma");
        write(root / "alpha" / "inner.txt", 1);
        write(root / "a.md", 10);
        write(root / "b.txt", 100);
        write(root / "c.txt", 1);
        browser.open(root);
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

TEST_CASE("details: Ctrl+3 shows the same folder, cursor and selection; every view switches") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);                   // alpha
    f.press(XKB_KEY_Down, {.shift = true});  // alpha, gamma
    const std::shared_ptr<Folder> folder = f.browser.columns().focused_folder();

    f.press(XKB_KEY_3, {.ctrl = true});
    REQUIRE(f.browser.mode() == Browser::Mode::details);
    CHECK(f.details.folder() == folder);  // the same object: nothing reloads
    CHECK(cursor_name(*folder) == "gamma");
    CHECK(folder->selected_names() == std::vector<std::string>{"alpha", "gamma"});

    f.press(XKB_KEY_Down);  // a.md, alone
    CHECK(folder->selected_names() == std::vector<std::string>{"a.md"});
    f.press(XKB_KEY_2, {.ctrl = true});  // straight to the grid
    REQUIRE(f.browser.mode() == Browser::Mode::grid);
    CHECK(f.browser.grid().folder() == folder);
    f.press(XKB_KEY_3, {.ctrl = true});  // and back
    CHECK(f.details.folder() == folder);
    CHECK(cursor_name(*folder) == "a.md");
    f.press(XKB_KEY_1, {.ctrl = true});
    REQUIRE(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.browser.columns().focused_folder() == folder);
}

TEST_CASE("details: sorting by a column, again reversed; the cursor stays on its entry") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    const std::shared_ptr<Folder> folder = f.details.folder();
    REQUIRE(folder != nullptr);
    CHECK(names(*folder) == std::vector<std::string>{"alpha", "gamma", "a.md", "b.txt", "c.txt"});
    f.press(XKB_KEY_End);  // c.txt

    f.details.sort_by(Field::size);
    CHECK(folder->order().key == SortKey::size);
    CHECK_FALSE(folder->order().descending);
    // Folders first, then 1, 10 and 100 bytes.
    CHECK(names(*folder) == std::vector<std::string>{"alpha", "gamma", "c.txt", "a.md", "b.txt"});
    CHECK(cursor_name(*folder) == "c.txt");
    CHECK(folder->cursor() == 2);

    f.details.sort_by(Field::size);  // the same title again: largest first
    CHECK(folder->order().descending);
    CHECK(names(*folder) == std::vector<std::string>{"gamma", "alpha", "b.txt", "a.md", "c.txt"});
    CHECK(cursor_name(*folder) == "c.txt");

    f.details.sort_by(Field::type);  // text/markdown before text/plain
    CHECK(names(*folder) == std::vector<std::string>{"alpha", "gamma", "a.md", "b.txt", "c.txt"});

    // The columns show the folder as sorted; a folder the details open is sorted the same way.
    f.details.sort_by(Field::modified);
    f.press(XKB_KEY_Home);  // alpha or gamma: either is a folder
    f.press(XKB_KEY_Return);
    REQUIRE(f.settle());
    CHECK(f.details.folder()->order().key == SortKey::modified);
    f.press(XKB_KEY_1, {.ctrl = true});
    CHECK(f.browser.columns().focused_folder()->order().key == SortKey::modified);
}

TEST_CASE("details: tree expansion keeps the root and selects children in their own folder") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    f.press(XKB_KEY_Down);   // alpha
    f.press(XKB_KEY_Right);  // expand, without changing location or cursor
    REQUIRE(f.settle());
    CHECK(f.details.root_folder()->path() == f.root);
    CHECK(f.details.folder()->path() == f.root);
    CHECK(cursor_name(*f.details.folder()) == "alpha");
    f.press(XKB_KEY_Right);  // first child
    CHECK(f.details.folder()->path() == f.root / "alpha");
    CHECK(cursor_name(*f.details.folder()) == "inner.txt");
    f.press(XKB_KEY_Down);  // next visible sibling of alpha
    CHECK(f.details.folder()->path() == f.root);
    CHECK(cursor_name(*f.details.folder()) == "gamma");
    f.press(XKB_KEY_Up);
    CHECK(cursor_name(*f.details.folder()) == "inner.txt");
    f.press(XKB_KEY_Left);  // select parent, then collapse
    CHECK(cursor_name(*f.details.folder()) == "alpha");
    f.press(XKB_KEY_Left);
    f.press(XKB_KEY_Down);
    CHECK(cursor_name(*f.details.folder()) == "gamma");
    f.press(XKB_KEY_Home);
    f.press(XKB_KEY_Return);  // navigation still opens the folder
    REQUIRE(f.settle());
    CHECK(f.details.root_folder()->path() == f.root / "alpha");
    f.press(XKB_KEY_BackSpace);
    REQUIRE(f.settle());
    CHECK(f.details.root_folder()->path() == f.root);
    CHECK(cursor_name(*f.details.folder()) == "alpha");
}

TEST_CASE("details: expanded children follow sort, hidden files and filesystem changes") {
    Fixture f;
    write(f.root / "alpha" / ".hidden", 5);
    write(f.root / "alpha" / "large.txt", 200);
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    f.press(XKB_KEY_Home);
    f.press(XKB_KEY_Right);
    REQUIRE(f.settle());
    f.details.sort_by(Field::size);
    f.press(XKB_KEY_Right);
    CHECK(cursor_name(*f.details.folder()) == "inner.txt");
    f.press(XKB_KEY_Down);
    CHECK(cursor_name(*f.details.folder()) == "large.txt");
    auto child = f.details.folder();
    f.details.set_show_hidden(true);
    CHECK(names(*child) == std::vector<std::string>{"inner.txt", ".hidden", "large.txt"});
    write(f.root / "alpha" / "new.txt", 400);
    REQUIRE(run_until(f.loop, [&] { return child->entries().size() == 4; }));
    f.press(XKB_KEY_Down);
    CHECK(cursor_name(*f.details.folder()) == "new.txt");
    fs::remove_all(f.root / "alpha");
    REQUIRE(run_until(f.loop, [&] { return f.details.root_folder()->entries().size() == 4; }));
    f.press(XKB_KEY_Home);
    CHECK(f.details.folder() == f.details.root_folder());
    CHECK(cursor_name(*f.details.folder()) == "gamma");
}

TEST_CASE("info pane: Ctrl+I hides and shows it in the grid and the details; nothing moves") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);                   // alpha
    f.press(XKB_KEY_Down, {.shift = true});  // alpha, gamma
    const std::shared_ptr<Folder> folder = f.browser.columns().focused_folder();
    CHECK(f.browser.info_visible());  // shown at start

    f.press(XKB_KEY_i, {.ctrl = true});
    CHECK_FALSE(f.browser.info_visible());
    CHECK_FALSE(f.browser.grid().info_visible());
    CHECK_FALSE(f.details.info_visible());
    // The columns keep theirs: it is their last column.
    CHECK(cursor_name(*folder) == "gamma");
    CHECK(folder->selected_names() == std::vector<std::string>{"alpha", "gamma"});

    f.press(XKB_KEY_3, {.ctrl = true});  // the setting goes along
    CHECK_FALSE(f.details.info_visible());
    f.press(XKB_KEY_I, {.ctrl = true});  // the keysym with Caps Lock on
    CHECK(f.browser.info_visible());
    CHECK(f.details.info_visible());
    CHECK(f.details.folder() == folder);
    CHECK(folder->selected_names() == std::vector<std::string>{"alpha", "gamma"});
}

TEST_CASE("info pane: its width stays within limits and leaves the view room") {
    Fixture f;
    rummage::views::InfoPane& pane = f.browser.info_pane();
    using rummage::views::InfoPane;
    CHECK(pane.width() == InfoPane::kWidth);
    pane.set_width(5000.0F);
    CHECK(pane.width() == InfoPane::kMaxWidth);
    pane.set_width(10.0F);
    CHECK(pane.width() == InfoPane::kMinWidth);

    pane.set_width(500.0F);
    CHECK(pane.width_in(2000.0F) == 500.0F);
    CHECK(pane.width_in(700.0F) == 700.0F - InfoPane::kMinContent);  // narrowed for the view
    CHECK(pane.width_in(500.0F) == 0.0F);  // not even the narrowest fits: left out
}

TEST_CASE("details: choosing and ordering columns keeps the selection and restores per folder") {
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    f.press(XKB_KEY_End);
    const auto folder = f.details.folder();
    const auto selection = folder->selected_names();
    f.details.set_column_visible(Field::type, false);
    f.details.set_column_visible(Field::owner, true);
    f.details.move_column(Field::owner, -1);
    CHECK(f.details.columns() ==
          std::vector<Field>{Field::name, Field::size, Field::owner, Field::modified, Field::mark});
    f.details.set_column_visible(Field::name, false);
    CHECK(f.details.column_visible(Field::name));
    f.details.set_columns(f.details.columns());  // Safe even with its own list.
    CHECK(f.details.column_visible(Field::owner));
    CHECK(folder->selected_names() == selection);
    const auto chosen = f.details.columns();
    const auto saved = f.browser.state();
    f.browser.open(f.root / "alpha");
    REQUIRE(f.settle());
    f.details.set_columns({Field::name, Field::permissions});
    f.browser.open(f.root);
    REQUIRE(f.settle());
    CHECK(f.details.columns() == chosen);
    const auto parsed = rummage::views::parse_state(rummage::views::format_state(saved), "test");
    REQUIRE(parsed.problems.empty());
    f.browser.apply(parsed.state);
    CHECK(f.details.columns() == chosen);
}

TEST_CASE("details: header chooser and sorting use the visible column layout") {
    rummage::ui::Offscreen gl{1200, 640};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    f.press(XKB_KEY_End);
    f.details.set_info_visible(false);
    const auto selection = f.details.folder()->selected_names();
    const auto draw = [&](float width) {
        renderer.begin_frame(1200, 640, 1, rummage::ui::Color::rgb(0));
        f.details.draw(renderer, {0, 0, width, 640}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    const auto click = [&](float x, rummage::ui::PointerButton button) {
        rummage::ui::PointerEvent event;
        event.kind = rummage::ui::PointerEvent::Kind::press;
        event.button = button;
        event.x = x;
        event.y = 15;
        f.details.pointer(event, Browser::Clock::now());
    };
    draw(1200);
    click(1100, rummage::ui::PointerButton::right);
    auto& menu = f.browser.context_menu();
    REQUIRE(menu.is_open());
    const auto& items = menu.items();
    const auto type = std::ranges::find(items, "column:type", &rummage::views::MenuItem::id);
    REQUIRE(type != items.end());
    CHECK(type->checked);
    CHECK(type->checkable);
    auto hide_type = type->action;
    menu.close();
    hide_type();
    CHECK_FALSE(f.details.column_visible(Field::type));
    CHECK(f.details.folder()->selected_names() == selection);
    f.details.set_columns({Field::name, Field::permissions, Field::owner});
    draw(1200);
    click(1140, rummage::ui::PointerButton::left);
    CHECK(f.details.order().key == SortKey::owner);
    click(1140, rummage::ui::PointerButton::left);
    CHECK(f.details.order().descending);
    CHECK(f.details.folder()->selected_names() == selection);
    draw(240);  // Optional columns temporarily do not fit; click sorts by Name.
    click(210, rummage::ui::PointerButton::left);
    CHECK(f.details.order().key == SortKey::name);
    CHECK(f.details.column_visible(Field::owner));
    draw(1200);
    click(1140, rummage::ui::PointerButton::left);
    CHECK(f.details.order().key == SortKey::owner);
    f.browser.open_view_menu(0, 0, true);
    const auto columns =
        std::ranges::find(menu.items(), "details_columns", &rummage::views::MenuItem::id);
    REQUIRE(columns != menu.items().end());
    // Status is offered only with a decorator (the git integration).
    CHECK(columns->children.size() == rummage::views::kDetailsColumnCount);
}

TEST_CASE("info pane: a folder's size, or a selection's, is added up once the cursor rests") {
    constexpr std::int32_t width = 1000;
    constexpr std::int32_t height = 600;
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(width, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    REQUIRE(f.settle());
    write(f.root / "alpha" / "more.txt", 2000);
    fs::create_directories(f.root / "alpha" / "deeper");
    write(f.root / "alpha" / "deeper" / "last.txt", 30);
    f.press(XKB_KEY_3, {.ctrl = true});
    if (!f.browser.info_visible()) {
        f.press(XKB_KEY_I, {.ctrl = true});
    }
    const rummage::views::InfoPane& pane = f.browser.info_pane();
    const auto draw = [&] {
        renderer.begin_frame(width, height, 1.0F, {});
        f.browser.draw(renderer, {0.0F, 0.0F, width, height}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };

    f.press(XKB_KEY_Down);  // alpha
    REQUIRE(cursor_name(*f.browser.columns().focused_folder()) == "alpha");
    draw();
    CHECK_FALSE(pane.folder_size());  // not before kMeasureDelay
    f.press(XKB_KEY_Down);            // gamma: alpha's count never starts
    draw();
    REQUIRE(run_until(f.loop, [&] { return pane.folder_size_done(); }));
    CHECK(pane.folder_size()->entries() == 1);  // gamma, empty
    CHECK(pane.folder_size()->bytes == 0);

    f.press(XKB_KEY_Up);  // alpha
    draw();
    REQUIRE(run_until(f.loop, [&] { return pane.folder_size_done(); }));
    const rummage::fs::Tally& alpha = *pane.folder_size();
    CHECK(alpha.files == 3);
    CHECK(alpha.folders == 2);
    CHECK(alpha.bytes == 1 + 2000 + 30);
    CHECK(alpha.complete());
    draw();  // the rows drawn with the sum

    // Several selected, folders among them: all of it added up.
    f.press(XKB_KEY_Down, {.shift = true});  // alpha, gamma
    draw();
    REQUIRE(run_until(f.loop, [&] { return pane.folder_size_done(); }));
    CHECK(pane.folder_size()->folders == 3);
    CHECK(pane.folder_size()->bytes == 1 + 2000 + 30);
    f.press(XKB_KEY_Down, {.shift = true});  // and a.md
    draw();
    REQUIRE(run_until(f.loop, [&] { return pane.folder_size_done(); }));
    CHECK(pane.folder_size()->files == 4);
    CHECK(pane.folder_size()->bytes == 1 + 2000 + 30 + 10);
    draw();

    // The same selection is invalidated when an entry's listed metadata changes.
    write(f.root / "a.md", 20);
    f.browser.columns().focused_folder()->refresh_entry("a.md");
    REQUIRE(run_until(f.loop, [&] {
        draw();
        return pane.folder_size_done() && pane.folder_size()->bytes == 1 + 2000 + 30 + 20;
    }));

    f.press(XKB_KEY_Down);  // b.txt alone: a file, no count
    draw();
    CHECK_FALSE(pane.folder_size());
    f.press(XKB_KEY_Down, {.shift = true});  // b.txt, c.txt: files only, their sizes as listed
    draw();
    CHECK_FALSE(pane.folder_size());
}

TEST_CASE("info pane: hiding a pane cancels its delayed folder count") {
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(1000, 600);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_3, {.ctrl = true});
    f.browser.set_info_visible(true);
    f.press(XKB_KEY_Down);  // alpha
    const auto draw = [&](float width) {
        renderer.begin_frame(1000, 600, 1.0F, {});
        f.browser.draw(renderer, {0, 0, width, 600}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    draw(1000);
    const auto& pane = f.browser.info_pane();
    CHECK_FALSE(pane.folder_size());
    SUBCASE("hidden explicitly") { f.browser.set_info_visible(false); }
    SUBCASE("window too narrow") { draw(300); }
    // Let the original debounce deadline pass: no count may start later.
    bool elapsed = false;
    const auto timer =
        f.loop.add_timer(InfoPane::kMeasureDelay + 20ms, 0ms, [&] { elapsed = true; });
    REQUIRE(run_until(f.loop, [&] { return elapsed; }));
    f.loop.remove(timer);
    CHECK_FALSE(pane.folder_size());
    CHECK_FALSE(pane.loading());
}
