#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/photo_library.hpp"
#include "scoped_env.hpp"
#include "temp_dir.hpp"

#ifdef RUMMAGE_PHOTOS_TEST_MODULE
namespace {
using namespace std::chrono_literals;
using rummage::core::EventLoop;
using rummage::ui::KeyEvent;

bool settle_photos(EventLoop& loop, const std::function<bool()>& predicate) {
    bool ready = false;
    const auto poll = loop.add_timer(5ms, 5ms, [&] {
        if (predicate()) {
            ready = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(8s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    return ready;
}

KeyEvent photo_key(xkb_keysym_t keysym, rummage::ui::Modifiers modifiers = {},
                   std::string text = {}) {
    KeyEvent result;
    result.keysym = keysym;
    result.modifiers = modifiers;
    result.text = std::move(text);
    return result;
}
}  // namespace

TEST_CASE("photo workspace: browser and picker stay inert until the library is explicitly opened") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", directory.path.string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{loop,  pool,       watcher, rummage::fs::MimeDatabase::load(),
                                    [] {}, [](auto) {}};
    const auto catalog = directory.path / "rummage/photos/catalog.sqlite3";
    browser.set_photos_enabled(true);
    CHECK(browser.photos_available());
    CHECK_FALSE(std::filesystem::exists(catalog));
    browser.set_picker_mode(true);
    CHECK_FALSE(browser.photos_available());
    browser.open_photo_library();
    CHECK_FALSE(browser.showing_photos());
    CHECK_FALSE(std::filesystem::exists(catalog));
    browser.set_picker_mode(false);
    browser.set_photos_enabled(false);
    browser.open_photo_library();
    CHECK_FALSE(browser.showing_photos());
    CHECK_FALSE(std::filesystem::exists(catalog));
    browser.set_photos_enabled(true);
    browser.open_photo_library();
    REQUIRE(browser.showing_photos());
    REQUIRE(settle_photos(loop, [&] { return !browser.photo_library()->loading(); }));
    CHECK(std::filesystem::exists(catalog));
    browser.set_photos_enabled(false);
    CHECK_FALSE(browser.showing_photos());
    CHECK(browser.photo_library() == nullptr);
    CHECK(std::filesystem::exists(catalog));
}

TEST_CASE("photo workspace: add folders through native input and keep equal filenames distinct") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto first = directory.path / "first";
    const auto second = directory.path / "second";
    std::filesystem::create_directories(first);
    std::filesystem::create_directories(second);
    std::filesystem::copy_file(std::filesystem::path{RUMMAGE_TEST_DATA} / "image/blocks.png",
                               first / "same.png");
    std::filesystem::copy_file(first / "same.png", second / "same.png");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::views::PhotoLibrary library{loop, pool, nullptr, [] {}, [](auto, auto) {}};
    library.open();
    library.add_folder(first);
    library.key(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return library.rows().size() == 1; }));
    library.add_folder(second);
    library.key(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return library.rows().size() == 2; }));
    CHECK(library.rows()[0]["filename"] == library.rows()[1]["filename"]);
    CHECK(library.rows()[0]["id"] != library.rows()[1]["id"]);
    library.key(photo_key(XKB_KEY_a, {.ctrl = true}));
    REQUIRE(library.selection().size() == 2);
    const auto ids = library.selection();
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.selection() == ids);
    const auto saved = library.state();
    rummage::views::PhotoLibrary reopened{loop, pool, nullptr, [] {}, [](auto, auto) {}};
    reopened.restore_state(saved);
    reopened.open();
    REQUIRE(settle_photos(loop, [&] { return !reopened.loading(); }));
    CHECK(reopened.selection() == ids);
    CHECK(reopened.rows().size() == 2);
    library.key(photo_key(XKB_KEY_Escape));
    CHECK(library.selection().empty());
    CHECK(library.is_open());
    // Search edits consume text and supersede earlier asynchronous responses.
    library.key(photo_key(XKB_KEY_f, {.ctrl = true}));
    library.paste("no match");
    library.key(photo_key(XKB_KEY_a, {.ctrl = true}));
    library.paste("same");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().size() == 2);
    CHECK(std::filesystem::exists(first / "same.png"));
    CHECK(std::filesystem::exists(second / "same.png"));
}

TEST_CASE("photo workspace: native gallery inspector and view switching preserve the working set") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Holiday";
    std::filesystem::create_directories(source);
    for (const auto& name : {"Holiday.png", "Beach.png", "Mountains.png"})
        std::filesystem::copy_file(std::filesystem::path{RUMMAGE_TEST_DATA} / "image/blocks.png",
                                   source / name);
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "photos-test");
    rummage::views::Browser browser{loop,  pool,        watcher, rummage::fs::MimeDatabase::load(),
                                    [] {}, [](auto) {}, cache};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    browser.open(source);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    const auto commands = browser.palette_items();
    const auto add = std::ranges::find(commands, "photos:add", &rummage::views::PaletteItem::id);
    REQUIRE(add != commands.end());
    add->action();
    REQUIRE(browser.showing_photos());
    auto* library = browser.photo_library();
    const auto draw = [&](const std::string& name, int width = 1100, int height = 820) {
        renderer.begin_frame(width, height, 1, {});
        browser.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                     rummage::theme::Palette{}, icons, rummage::views::Browser::Clock::now());
        renderer.end_frame();
        if (const char* frames = std::getenv("RUMMAGE_PHOTO_FRAMES")) {
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream output{std::filesystem::path{frames} / (name + ".ppm"), std::ios::binary};
            output << "P6\n" << width << ' ' << height << "\n255\n";
            for (int y = height - 1; y >= 0; --y)
                for (int x = 0; x < width; ++x)
                    output.write(reinterpret_cast<const char*>(pixels.data() + (y * width + x) * 4),
                                 3);
            REQUIRE(output.good());
        }
    };
    const auto click = [&](rummage::ui::Rect bounds) {
        REQUIRE_FALSE(bounds.empty());
        for (const auto kind :
             {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
            browser.pointer({.kind = kind,
                             .x = bounds.x + bounds.width / 2,
                             .y = bounds.y + bounds.height / 2,
                             .button = rummage::ui::PointerButton::left},
                            rummage::views::Browser::Clock::now());
    };
    draw("source-setup");
    click(library->control_bounds("Add folder"));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 3 && !library->loading(); }));
    const auto second_source = directory.path / "Other trip";
    std::filesystem::create_directories(second_source);
    std::filesystem::copy_file(source / "Beach.png", second_source / "Beach.png");
    browser.open_photo_library(second_source);
    CHECK(browser.key(photo_key(XKB_KEY_Return), rummage::views::Browser::Clock::now()));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 4 && !library->loading(); }));
    draw("gallery-loading");
    REQUIRE(settle_photos(loop, [&] {
        draw("gallery");
        return !browser.loading();
    }));
    click(library->row_bounds(0));
    REQUIRE(library->selection().size() == 1);
    const auto chosen = library->selection();
    const auto before_preview = library->state();
    CHECK(browser.key(photo_key(XKB_KEY_Return), rummage::views::Browser::Clock::now()));
    REQUIRE(settle_photos(loop, [&] {
        draw("quick-look-loading");
        return browser.quick_look().is_open() && !browser.loading();
    }));
    draw("quick-look");
    CHECK(browser.key(photo_key(XKB_KEY_Escape), rummage::views::Browser::Clock::now()));
    CHECK_FALSE(browser.quick_look().is_open());
    CHECK(library->selection() == chosen);
    CHECK(library->state() == before_preview);
    draw("selected");
    click(library->control_bounds("Edit selected"));
    draw("inspector");
    CHECK(browser.key(photo_key(XKB_KEY_a, {.ctrl = true}), rummage::views::Browser::Clock::now()));
    CHECK(browser.key(photo_key(XKB_KEY_NoSymbol, {}, "Été au bord de la mer — 夏"),
                      rummage::views::Browser::Clock::now()));
    CHECK(browser.key(photo_key(XKB_KEY_Return), rummage::views::Browser::Clock::now()));
    REQUIRE(settle_photos(loop, [&] {
        const auto result = library->client()->request({{"op", "query"}});
        return result.value("ok", false) &&
               result["photos"][0].value("description", "") == "Été au bord de la mer — 夏";
    }));
    const auto press = [&](KeyEvent event) {
        CHECK(browser.key(event, rummage::views::Browser::Clock::now()));
    };
    // The same native fields retain Unicode tags and timezone-unknown date overrides.
    press(photo_key(XKB_KEY_Tab));
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_NoSymbol, {}, "5"));
    press(photo_key(XKB_KEY_Return));
    press(photo_key(XKB_KEY_Tab));
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_NoSymbol, {}, "2024-06-12T14:30:00"));
    press(photo_key(XKB_KEY_Return));
    press(photo_key(XKB_KEY_Tab));
    press(photo_key(XKB_KEY_NoSymbol, {}, "été, 家族"));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        const auto result = library->client()->request({{"op", "query"}});
        if (!result.value("ok", false))
            return false;
        const auto row = result["photos"][0];
        return row.value("rating", 0) == 5 && row["capture_date"] == "2024-06-12T14:30:00" &&
               row.value("tags", nlohmann::json::array()).size() == 2;
    }));
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    draw("inspector-saved");
    press(photo_key(XKB_KEY_Tab, {.shift = true}));
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_BackSpace));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        const auto result = library->client()->request({{"op", "query"}});
        return result.value("ok", false) && result["photos"][0]["capture_date"].is_null();
    }));
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    draw("inspector-restored-date");
    draw("inspector-small", 680, 380);
    press(photo_key(XKB_KEY_Page_Down));
    draw("inspector-small-scrolled", 680, 380);
    CHECK(library->control_bounds("Done").bottom() <= 380);
    click(library->control_bounds("Done"));
    draw("gallery-edited");
    CHECK(library->selection() == chosen);
    click(library->control_bounds("Details view"));
    draw("details");
    CHECK(library->selection() == chosen);
    CHECK(library->rows().size() == 4);
    CHECK_FALSE(library->background_status().empty());
    click(library->control_bounds("Pause background work"));
    REQUIRE(settle_photos(loop, [&] {
        return library->client()->request({{"op", "background_state"}}).value("paused", false) &&
               !library->loading();
    }));
    draw("background-paused");
    // The native browser's shared watcher notices a new photo, but global pause prevents the
    // queued reconciliation from beginning until the user resumes it.
    std::filesystem::copy_file(source / "Beach.png", source / "Captured.png");
    REQUIRE(settle_photos(loop, [&] {
        const auto jobs =
            library->client()->request({{"op", "jobs"}}).value("jobs", nlohmann::json::array());
        return std::ranges::any_of(
            jobs, [](const auto& job) { return job.value("state", "") == "queued"; });
    }));
    CHECK(library->rows().size() == 4);
    click(library->control_bounds("Resume background work"));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 5 && !library->loading(); }));
    CHECK(library->selection() == chosen);
    draw("watcher-new-photo");
    click(library->control_bounds("Pace: Normal"));
    REQUIRE(settle_photos(loop, [&] {
        return library->client()->request({{"op", "background_state"}}).value("pace", "") ==
                   "gentle" &&
               !library->loading();
    }));
    draw("background-gentle");
    click(library->control_bounds("Pace: Gentle"));
    REQUIRE(settle_photos(loop, [&] {
        return library->client()->request({{"op", "background_state"}}).value("pace", "") ==
                   "normal" &&
               !library->loading();
    }));
    const auto disconnected = directory.path / "Disconnected original folder";
    std::filesystem::rename(source, disconnected);
    REQUIRE(settle_photos(loop, [&] {
        return std::ranges::count_if(library->rows(), [](const auto& photo) {
                   return photo.value("availability", "") == "offline";
               }) == 4;
    }));
    CHECK(library->selection() == chosen);
    draw("offline-loading");
    REQUIRE(settle_photos(loop, [&] {
        draw("offline-loading");
        return !browser.loading();
    }));
    draw("offline-cached-details");
    std::filesystem::rename(disconnected, source);
    for (const auto& name : {"Holiday.png", "Beach.png", "Mountains.png"})
        CHECK(std::filesystem::file_size(source / name) ==
              std::filesystem::file_size(std::filesystem::path{RUMMAGE_TEST_DATA} /
                                         "image/blocks.png"));
}

TEST_CASE(
    "photo workspace: large duplicate groups scroll full locations and capture the exact chosen "
    "scope") {
    rummage::ui::Offscreen offscreen{1000, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source =
        directory.path / "Photo archive" / std::string(70, 'a') / std::string(70, 'b');
    std::filesystem::create_directories(source);
    for (int i = 0; i < 50; ++i)
        std::filesystem::copy_file(std::filesystem::path{RUMMAGE_TEST_DATA} / "image/blocks.png",
                                   source / (std::to_string(i) + ".png"));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::views::PhotoLibrary library{loop, pool, nullptr, [] {}, [](auto, auto) {}};
    nlohmann::json captured;
    library.set_file_action([&](auto request) { captured = std::move(request); });
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    const auto draw = [&](const std::string& name) {
        renderer.begin_frame(1000, 700, 1, {});
        library.draw(renderer, {0, 0, 1000, 700}, rummage::theme::Palette{});
        renderer.end_frame();
        if (const char* frames = std::getenv("RUMMAGE_PHOTO_FRAMES")) {
            std::vector<unsigned char> pixels(1000 * 700 * 4);
            glReadPixels(0, 0, 1000, 700, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream output{std::filesystem::path{frames} / (name + ".ppm"), std::ios::binary};
            output << "P6\n1000 700\n255\n";
            for (int y = 699; y >= 0; --y)
                for (int x = 0; x < 1000; ++x)
                    output.write(reinterpret_cast<const char*>(pixels.data() + (y * 1000 + x) * 4),
                                 3);
            REQUIRE(output.good());
        }
    };
    const auto click = [&](std::string_view label) {
        const auto bounds = library.control_bounds(label);
        REQUIRE_FALSE(bounds.empty());
        library.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                         .x = bounds.x + bounds.width / 2,
                         .y = bounds.y + bounds.height / 2,
                         .button = rummage::ui::PointerButton::left});
    };
    library.open();
    library.add_folder(source);
    library.key(photo_key(XKB_KEY_Return));
    const bool imported = settle_photos(loop, [&] { return library.rows().size() == 50; });
    INFO("Loaded rows: " << library.rows().size());
    INFO("Catalog jobs: " << library.client()->request({{"op", "jobs"}}).dump());
    INFO("Catalog sources: " << library.client()->request({{"op", "sources"}}).dump());
    draw("large-library");
    REQUIRE(imported);
    const auto initial_row = library.row_bounds(0);
    REQUIRE_FALSE(initial_row.empty());
    library.pointer({.kind = rummage::ui::PointerEvent::Kind::press,
                     .x = initial_row.x + 20,
                     .y = initial_row.y + 20,
                     .button = rummage::ui::PointerButton::left});
    library.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 500, .y = 400, .scroll_y = 100000});
    draw("large-library-scrolled");
    const auto collection_state = library.state();
    click("Activity");
    draw("large-activity");
    library.key(photo_key(XKB_KEY_Down));
    CHECK(library.state() == collection_state);
    click("Library");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    draw("large-library-returned");
    CHECK(library.state() == collection_state);
    click("Review");
    draw("duplicate-review");
    click("Find exact duplicates");
    // Groups arrive incrementally. Wait for the complete fixture before capturing the review
    // scope; opening an earlier group correctly keeps only that earlier set of candidates.
    REQUIRE(settle_photos(loop, [&] {
        const auto result = library.client()->request({{"op", "duplicates"}});
        const auto groups = result.value("groups", nlohmann::json::array());
        return !groups.empty() &&
               groups.front().value("photos", nlohmann::json::array()).size() == 50;
    }));
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    draw("duplicate-groups");
    click("Review these copies");
    draw("duplicates-first");
    REQUIRE_FALSE(library.control_bounds("Keep 1 · Trash 49").empty());
    library.pointer(
        {.kind = rummage::ui::PointerEvent::Kind::scroll, .x = 500, .y = 350, .scroll_y = 100000});
    draw("duplicates-last");
    click("Trash");
    draw("duplicates-two-keepers");
    click("Keep 2 · Trash 48");
    REQUIRE(captured.is_object());
    CHECK(captured["kind"] == "trash");
    CHECK(captured["keeper_ids"].size() == 2);
    CHECK(captured["ids"].size() == 48);
    CHECK(captured["reviewed"].size() == 50);
    for (const auto& id : captured["keeper_ids"])
        CHECK(std::ranges::find(captured["ids"], id) == captured["ids"].end());
    // This UI test captures the verified-operation handoff; filesystem behavior is covered by
    // Browser/TransferQueue recovery tests. Reviewing alone never changes originals.
    for (int i = 0; i < 50; ++i)
        CHECK(std::filesystem::exists(source / (std::to_string(i) + ".png")));
}
#endif
