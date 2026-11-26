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
using rummage::views::Browser;
using rummage::views::PhotoLibrary;

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

void invoke(std::vector<rummage::views::MenuItem> items, std::string_view id) {
    const auto found = std::ranges::find(items, id, &rummage::views::MenuItem::id);
    REQUIRE(found != items.end());
    REQUIRE(found->enabled);
    REQUIRE(static_cast<bool>(found->action));
    found->action();
}

void capture(const std::string& name, int width, int height) {
    const char* frames = std::getenv("RUMMAGE_PHOTO_FRAMES");
    if (!frames || name.empty())
        return;
    std::filesystem::create_directories(frames);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream output{std::filesystem::path{frames} / (name + ".ppm"), std::ios::binary};
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            output.write(reinterpret_cast<const char*>(pixels.data() + (y * width + x) * 4), 3);
    REQUIRE(output.good());
}

void fixture(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::copy_file(std::filesystem::path{RUMMAGE_TEST_DATA} / "image/blocks.png", path);
}
}  // namespace

TEST_CASE("photo workspace: ordinary browser and picker remain inert until explicit activation") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", directory.path.string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    const auto catalog = directory.path / "rummage/photos/catalog.sqlite3";
    browser.set_photos_enabled(true);
    CHECK(browser.photos_available());
    CHECK_FALSE(std::filesystem::exists(catalog));
    browser.set_picker_mode(true);
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

TEST_CASE(
    "photo workspace: real collection Folder keeps equal filenames distinct and query state "
    "stable") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto first = directory.path / "first", second = directory.path / "second";
    fixture(first / "same.png");
    fixture(second / "same.png");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    PhotoLibrary library{loop, pool, watcher, mime, [] {}};
    library.open();
    library.add_folder(first);
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return library.rows().size() == 1; }));
    library.add_folder(second);
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return library.rows().size() == 2; }));
    const auto folder = library.folder();
    CHECK(folder->is_collection());
    CHECK(folder->path().empty());
    REQUIRE(folder->entries().size() == 2);
    CHECK(folder->entries()[0].name == folder->entries()[1].name);
    CHECK(folder->entries()[0].key() != folder->entries()[1].key());
    CHECK(folder->path_of(folder->entries()[0]) != folder->path_of(folder->entries()[1]));
    folder->set_cursor(1);
    const auto chosen = library.selected_ids();
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.selected_ids() == chosen);
    const auto saved = library.state();
    PhotoLibrary restored{loop, pool, watcher, mime, [] {}};
    restored.restore_state(saved);
    restored.open();
    REQUIRE(settle_photos(loop, [&] { return !restored.loading(); }));
    CHECK(restored.selected_ids() == chosen);
    CHECK(restored.folder()->selected_count() == 1);
    library.set_query("absent");
    library.set_query("same");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().size() == 2);
    CHECK(library.selected_ids().empty());
    rummage::core::PickerRequest picker_request;
    std::function<void(std::optional<std::filesystem::path>)> chosen_path;
    library.set_path_chooser([&](const auto& request, auto completion) {
        picker_request = request;
        chosen_path = std::move(completion);
    });
    invoke(library.menu_items(), "photos:export");
    CHECK(picker_request.mode == rummage::core::PickerRequest::Mode::save);
    CHECK_FALSE(picker_request.directory);
    CHECK(picker_request.name == "rummage-photo-annotations.json");
    CHECK_FALSE(library.has_dialog());
    const auto exported = directory.path / "annotations.json";
    chosen_path(std::nullopt);
    CHECK_FALSE(std::filesystem::exists(exported));
    invoke(library.menu_items(), "photos:export");
    chosen_path(exported);
    REQUIRE(settle_photos(loop, [&] {
        return library.status().find("Photo annotations exported") != std::string::npos;
    }));
    REQUIRE(std::filesystem::exists(exported));
    const auto id = library.rows().front().at("id");
    REQUIRE(library.client()
                ->request({{"op", "edit"}, {"ids", {id}}, {"description", "After export"}})
                .value("ok", false));
    invoke(library.menu_items(), "photos:restore");
    CHECK(picker_request.mode == rummage::core::PickerRequest::Mode::open);
    CHECK_FALSE(picker_request.directory);
    chosen_path(exported);
    REQUIRE(settle_photos(loop, [&] {
        const auto rows = library.client()->request({{"op", "query"}}).at("photos");
        return std::ranges::any_of(rows, [&](const auto& row) {
            return row.at("id") == id && row.value("description", "") != "After export";
        });
    }));
    CHECK(std::filesystem::exists(first / "same.png"));
    CHECK(std::filesystem::exists(second / "same.png"));
}

TEST_CASE(
    "photo workspace: uses native browser frames folder chooser grid details QuickLook and "
    "Properties") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Holiday";
    for (const auto* name : {"Holiday.png", "Beach.png", "Mountains.png"})
        fixture(source / name);
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "photos-test");
    Browser browser{loop,  pool,        watcher, rummage::fs::MimeDatabase::load(),
                    [] {}, [](auto) {}, cache};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&](const std::string& name = "", int width = 1100, int height = 820) {
        const auto frame = [&] {
            const rummage::theme::Palette palette;
            renderer.begin_frame(width, height, 1, palette.surface);
            browser.draw(renderer, {0, 0, static_cast<float>(width), static_cast<float>(height)},
                         palette, icons, Browser::Clock::now());
            renderer.end_frame();
        };
        frame();
        if (!name.empty())
            REQUIRE(settle_photos(loop, [&] {
                frame();
                return !browser.animating();
            }));
        capture(name, width, height);
    };
    const auto press = [&](KeyEvent key) { CHECK(browser.key(key, Browser::Clock::now())); };
    const auto click = [&](rummage::ui::Rect rect) {
        REQUIRE_FALSE(rect.empty());
        for (auto kind :
             {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
            browser.pointer({.kind = kind,
                             .x = rect.x + rect.width / 2,
                             .y = rect.y + rect.height / 2,
                             .button = rummage::ui::PointerButton::left},
                            Browser::Clock::now());
    };
    browser.set_places({{.label = "Home", .path = directory.path, .icon = "home"},
                        {.label = "Pictures", .path = source, .icon = "photo"}});
    browser.open(source);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    const auto normal_tab = browser.active_tab();
    std::function<void(std::filesystem::path)> chosen_folder;
    browser.set_folder_chooser(
        [&](const auto&, auto chosen) { chosen_folder = std::move(chosen); });
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(browser.tab_count() == 2);
    const auto photo_tab = browser.active_tab();
    library->add_folder();
    REQUIRE(static_cast<bool>(chosen_folder));
    CHECK_FALSE(library->has_dialog());
    chosen_folder(source);
    REQUIRE(library->has_dialog());
    draw("native-source-review");
    click(library->control_bounds("Add folder"));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 3 && !library->loading(); }));
    const auto second_source = directory.path / "Other trip";
    fixture(second_source / "Beach.png");
    browser.open_photo_library(second_source);
    press(photo_key(XKB_KEY_Return));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 4 && !library->loading(); }));
    browser.set_mode(Browser::Mode::grid);
    draw();
    REQUIRE(settle_photos(loop, [&] {
        draw();
        return !browser.loading();
    }));
    CHECK(browser.sidebar_visible());
    CHECK(browser.grid().folder() == library->folder());
    CHECK(browser.tab_titles()[photo_tab] == "Photo Library");
    press(photo_key(XKB_KEY_Home));
    draw("native-gallery");
    REQUIRE(library->folder()->selected_count() == 1);
    const auto selected = library->selected_ids();
    const auto before = library->state();
    press(photo_key(XKB_KEY_space));
    REQUIRE(settle_photos(loop, [&] {
        draw();
        return browser.quick_look().is_open() && !browser.loading();
    }));
    draw("native-quick-look");
    press(photo_key(XKB_KEY_Escape));
    CHECK(library->selected_ids() == selected);
    CHECK(library->state() == before);
    invoke(library->context_menu_items(), "photos:edit");
    REQUIRE(browser.properties_dialog().is_open());
    draw("native-properties");
    // Existing PropertiesDialog keyboard focus order reaches the actual native text field.
    for (int i = 0; i < 3; ++i) {
        press(photo_key(XKB_KEY_Tab));
        draw();
    }
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_NoSymbol, {}, "Été au bord de la mer — 夏"));
    for (int i = 0; i < 2; ++i) {
        press(photo_key(XKB_KEY_Tab));
        draw();
    }
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_NoSymbol, {}, "5"));
    for (int i = 0; i < 2; ++i) {
        press(photo_key(XKB_KEY_Tab));
        draw();
    }
    press(photo_key(XKB_KEY_a, {.ctrl = true}));
    press(photo_key(XKB_KEY_NoSymbol, {}, "2024-06-12T14:30:00"));
    press(photo_key(XKB_KEY_Tab));
    draw();
    press(photo_key(XKB_KEY_NoSymbol, {}, "été, 家族"));
    draw("native-properties-edited");
    click(browser.properties_dialog().control_bounds("apply"));
    REQUIRE(settle_photos(loop, [&] {
        const auto rows =
            library->client()->request({{"op", "query"}}).value("photos", nlohmann::json::array());
        return std::ranges::any_of(rows, [&](const auto& row) {
            return row["id"] == selected.front() &&
                   row.value("description", "") == "Été au bord de la mer — 夏" &&
                   row.value("rating", 0) == 5 &&
                   row.value("tags", nlohmann::json::array()).size() == 2;
        });
    }));
    REQUIRE(settle_photos(
        loop, [&] { return !browser.properties_dialog().saving() && !library->loading(); }));
    draw("native-properties-saved");
    CHECK_FALSE(browser.properties_dialog().is_open());
    browser.set_mode(Browser::Mode::details);
    draw("native-details");
    CHECK(browser.mode() == Browser::Mode::details);
    CHECK(browser.details().root_folder() == library->folder());
    CHECK(library->selected_ids() == selected);
    browser.select_tab(normal_tab);
    draw("native-original-tab");
    CHECK_FALSE(browser.showing_photos());
    CHECK(browser.picker_folder()->path() == source);
    browser.select_tab(photo_tab);
    draw("native-returned-tab");
    CHECK(browser.showing_photos());
    CHECK(library->selected_ids() == selected);
    {
        const auto saved_state = browser.state();
        const auto parsed = rummage::views::parse_state(rummage::views::format_state(saved_state),
                                                        "native-photo-session.toml");
        REQUIRE(parsed.problems.empty());
        Browser reopened{loop,  pool,        watcher, rummage::fs::MimeDatabase::load(),
                         [] {}, [](auto) {}, cache};
        reopened.apply(parsed.state);
        REQUIRE(reopened.restore_session(parsed.state));
        REQUIRE(reopened.showing_photos());
        REQUIRE(settle_photos(loop, [&] {
            return reopened.photo_library()->rows().size() == 4 &&
                   !reopened.photo_library()->loading();
        }));
        CHECK(reopened.mode() == Browser::Mode::details);
        CHECK(reopened.photo_library()->selected_ids() == selected);
        REQUIRE(reopened.photo_library()->folder()->cursor_entry());
        CHECK(reopened.photo_library()->folder()->cursor_entry()->key() ==
              library->folder()->cursor_entry()->key());
        CHECK(reopened.details().columns() == browser.details().columns());
        CHECK(reopened.details().widths() == browser.details().widths());
    }
    invoke(library->menu_items(), "photos:pause");
    REQUIRE(settle_photos(loop, [&] {
        return library->client()->request({{"op", "background_state"}}).value("paused", false) &&
               !library->loading();
    }));
    fixture(source / "Captured.png");
    REQUIRE(settle_photos(loop, [&] {
        const auto jobs =
            library->client()->request({{"op", "jobs"}}).value("jobs", nlohmann::json::array());
        return std::ranges::any_of(
            jobs, [](const auto& row) { return row.value("state", "") == "queued"; });
    }));
    CHECK(library->rows().size() == 4);
    invoke(library->menu_items(), "photos:pause");
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 5 && !library->loading(); }));
    draw("native-watched-photo");
    library->show(PhotoLibrary::Destination::sources);
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    draw();
    press(photo_key(XKB_KEY_Home));
    const auto source_properties = library->properties(true);
    CHECK(source_properties.ids.empty());
    CHECK(source_properties.path.empty());
    CHECK(source_properties.entry.identity.starts_with("source:"));
    CHECK_FALSE(source_properties.rows.empty());
    draw("native-sources");
    library->show(PhotoLibrary::Destination::activity);
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    draw();
    press(photo_key(XKB_KEY_Home));
    CHECK_FALSE(library->properties(true).rows.empty());
    draw("native-activity");
    library->show(PhotoLibrary::Destination::library);
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    const auto disconnected = directory.path / "Disconnected";
    std::filesystem::rename(source, disconnected);
    REQUIRE(settle_photos(loop, [&] {
        return std::ranges::count_if(library->rows(), [](const auto& row) {
                   return row.value("availability", "") == "offline";
               }) == 4;
    }));
    draw();
    REQUIRE(settle_photos(loop, [&] {
        draw();
        return !browser.loading();
    }));
    draw("native-offline-details");
    CHECK(library->selected_ids() == selected);
    std::filesystem::rename(disconnected, source);
}

TEST_CASE(
    "photo workspace: duplicate candidates use native Details selection and captured "
    "confirmation") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source =
        directory.path / "Photo archive" / std::string(70, 'a') / std::string(70, 'b');
    for (int i = 0; i < 50; ++i)
        fixture(source / (std::to_string(i) + ".png"));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&](const std::string& name = "") {
        const auto frame = [&] {
            const rummage::theme::Palette palette;
            renderer.begin_frame(1100, 820, 1, palette.surface);
            browser.draw(renderer, {0, 0, 1100, 820}, palette, icons, Browser::Clock::now());
            renderer.end_frame();
        };
        frame();
        if (!name.empty())
            REQUIRE(settle_photos(loop, [&] {
                frame();
                return !browser.animating();
            }));
        capture(name, 1100, 820);
    };
    browser.set_places({{.label = "Home", .path = directory.path, .icon = "home"},
                        {.label = "Pictures", .path = source, .icon = "photo"}});
    browser.open(directory.path);
    browser.open_photo_library(source);
    auto* library = browser.photo_library();
    REQUIRE(library);
    library->key_dialog(photo_key(XKB_KEY_Return));
    const bool imported = settle_photos(loop, [&] { return library->rows().size() == 50; });
    INFO("Jobs: " << library->client()->request({{"op", "jobs"}}).dump());
    REQUIRE(imported);
    library->show(PhotoLibrary::Destination::review);
    invoke(library->menu_items(), "photos:find-duplicates");
    REQUIRE(settle_photos(loop, [&] {
        const auto groups = library->client()
                                ->request({{"op", "duplicates"}})
                                .value("groups", nlohmann::json::array());
        return !groups.empty() &&
               groups.front().value("photos", nlohmann::json::array()).size() == 50;
    }));
    library->refresh();
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    draw("native-duplicate-groups");
    CHECK(browser.mode() == Browser::Mode::details);
    CHECK(browser.key(photo_key(XKB_KEY_Home), Browser::Clock::now()));
    CHECK(browser.key(photo_key(XKB_KEY_Return), Browser::Clock::now()));
    REQUIRE(library->destination() == PhotoLibrary::Destination::duplicate_group);
    browser.set_mode(Browser::Mode::details);
    draw("native-duplicate-candidates");
    CHECK(browser.details().root_folder() == library->folder());
    CHECK(browser.key(photo_key(XKB_KEY_End), Browser::Clock::now()));
    draw("native-duplicate-last");
    invoke(library->context_menu_items(), "photos:keep");
    CHECK(library->status().find("2 kept") != std::string::npos);
    nlohmann::json captured;
    library->set_file_action([&](auto request) { captured = std::move(request); });
    invoke(library->menu_items(), "photos:trash-marked");
    draw("native-duplicate-confirmation");
    CHECK(browser.key(photo_key(XKB_KEY_Return), Browser::Clock::now()));
    REQUIRE(captured.is_object());
    CHECK(captured["keeper_ids"].size() == 2);
    CHECK(captured["ids"].size() == 48);
    CHECK(captured["reviewed"].size() == 50);
    for (int i = 0; i < 50; ++i)
        CHECK(std::filesystem::exists(source / (std::to_string(i) + ".png")));
}

TEST_CASE(
    "photo workspace: create album captures thirteen selected photos and empty albums have exits") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Album pictures";
    for (int i = 0; i < 14; ++i)
        fixture(source / (std::to_string(i) + ".png"));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    browser.set_places({{.label = "Home", .path = directory.path, .icon = "home"},
                        {.label = "Pictures", .path = source, .icon = "photo"}});
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    const auto draw = [&](const std::string& name = "") {
        const auto frame = [&] {
            const rummage::theme::Palette palette;
            renderer.begin_frame(1100, 820, 1, palette.surface);
            browser.draw(renderer, {0, 0, 1100, 820}, palette, icons, Browser::Clock::now());
            renderer.end_frame();
        };
        frame();
        if (!name.empty())
            REQUIRE(settle_photos(loop, [&] {
                frame();
                return !browser.animating();
            }));
        capture(name, 1100, 820);
    };
    const auto press = [&](KeyEvent key) { REQUIRE(browser.key(key, Browser::Clock::now())); };
    browser.open(directory.path);
    browser.open_photo_library(source);
    auto* library = browser.photo_library();
    REQUIRE(library);
    press(photo_key(XKB_KEY_Return));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 14 && !library->loading(); }));
    browser.set_mode(Browser::Mode::details);
    draw();
    press(photo_key(XKB_KEY_Home));
    for (int i = 0; i < 12; ++i)
        press(photo_key(XKB_KEY_Down, {.shift = true}));
    const auto captured_ids = library->selected_ids();
    REQUIRE(captured_ids.size() == 13);
    invoke(library->menu_items(), "photos:create-album");
    REQUIRE(library->has_dialog());
    press(photo_key(XKB_KEY_NoSymbol, {}, "Thirteen selected"));
    draw("native-album-create-thirteen");
    // A changed collection cursor while the dialog is open must not change its captured scope.
    library->folder()->set_cursor(13);
    REQUIRE(library->selected_ids().size() == 1);
    CHECK(library->selected_ids() != captured_ids);
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        const auto albums = library->client()->request({{"op", "albums"}}).at("albums");
        return std::ranges::any_of(albums, [](const auto& album) {
            return album.value("name", "") == "Thirteen selected" && album.value("count", 0) == 13;
        });
    }));
    const auto open_album = [&](std::string_view name) {
        library->show(PhotoLibrary::Destination::albums);
        REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
        const auto& entries = library->folder()->entries();
        const auto found = std::ranges::find(entries, name, &rummage::fs::Entry::name);
        REQUIRE(found != entries.end());
        draw();
        press(photo_key(XKB_KEY_F6, {.shift = true}));
        press(photo_key(XKB_KEY_Home));
        for (auto it = entries.begin(); it != found; ++it)
            press(photo_key(XKB_KEY_Down));
        press(photo_key(XKB_KEY_Return));
        REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
        CHECK(library->destination() == PhotoLibrary::Destination::library);
    };
    open_album("Thirteen selected");
    REQUIRE(library->rows().size() == 13);
    CHECK(library->title().find("Thirteen selected") != std::string::npos);
    std::set<PhotoLibrary::Id> actual_ids;
    for (const auto& row : library->rows())
        actual_ids.insert(row.at("id").get<PhotoLibrary::Id>());
    CHECK(actual_ids == captured_ids.get<std::set<PhotoLibrary::Id>>());
    draw("native-album-thirteen");
    invoke(library->menu_items(), "photos:destination:0");
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 14 && !library->loading(); }));
    invoke(library->menu_items(), "photos:clear-selection");
    REQUIRE(library->selected_ids().empty());
    invoke(library->menu_items(), "photos:create-album");
    press(photo_key(XKB_KEY_NoSymbol, {}, "Empty album"));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        const auto albums = library->client()->request({{"op", "albums"}}).at("albums");
        return std::ranges::any_of(albums, [](const auto& album) {
            return album.value("name", "") == "Empty album" && album.value("count", -1) == 0;
        });
    }));
    for (const auto route : {"sidebar", "back", "parent", "menu"}) {
        INFO("Return route: " << route);
        open_album("Empty album");
        REQUIRE(library->rows().empty());
        draw("native-empty-album");
        if (std::string_view{route} == "sidebar") {
            const auto bounds = browser.sidebar().place_icon_rect({});
            REQUIRE(bounds);
            for (auto kind :
                 {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
                browser.pointer({.kind = kind,
                                 .x = bounds->x + bounds->width / 2,
                                 .y = bounds->y + bounds->height / 2,
                                 .button = rummage::ui::PointerButton::left},
                                Browser::Clock::now());
        } else if (std::string_view{route} == "back")
            press(photo_key(XKB_KEY_Left, {.alt = true}));
        else if (std::string_view{route} == "parent")
            press(photo_key(XKB_KEY_Up, {.alt = true}));
        else
            invoke(library->menu_items(), "photos:destination:0");
        REQUIRE(settle_photos(loop,
                              [&] { return library->rows().size() == 14 && !library->loading(); }));
        CHECK(browser.showing_photos());
        CHECK(library->query_text().empty());
        CHECK(library->at_all_photos());
    }
    draw("native-album-return-all");
    for (int i = 0; i < 14; ++i)
        CHECK(std::filesystem::exists(source / (std::to_string(i) + ".png")));
}
#endif
