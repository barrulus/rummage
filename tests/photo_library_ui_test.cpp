#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/i18n.hpp"
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

bool settle_photos(EventLoop& loop, const std::function<bool()>& predicate,
                   std::chrono::milliseconds budget = 8s) {
    bool ready = false;
    const auto poll = loop.add_timer(5ms, 5ms, [&] {
        if (predicate()) {
            ready = true;
            loop.quit(0);
        }
    });
    const auto timeout = loop.add_timer(budget, 0ms, [&] { loop.quit(1); });
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
    folder->set_cursor(std::nullopt);
    folder->select_band({}, std::vector<std::size_t>{0, 1});
    CHECK(folder->cursor_entry() == nullptr);
    CHECK(library.properties(true).ids.empty());
    CHECK(library.properties().ids.size() == 2);
    folder->set_cursor(1);
    const auto initial_properties = library.properties(true);
    REQUIRE(initial_properties.ids.size() == 1);
    const auto chosen = library.selected_ids();
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.selected_ids() == chosen);
    REQUIRE(library.client()
                ->request({{"op", "edit"}, {"ids", chosen}, {"description", "Updated annotations"}})
                .value("ok", false));
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.properties(true).annotations.description == "Updated annotations");
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
    REQUIRE(settle_photos(loop, [&] { return library.has_dialog(); }));
    library.key_dialog(photo_key(XKB_KEY_Escape));
    CHECK_FALSE(library.has_dialog());
    CHECK(library.client()
              ->request({{"op", "query"}})
              .at("photos")
              .front()
              .value("description", "") == "After export");
    invoke(library.menu_items(), "photos:restore");
    chosen_path(exported);
    REQUIRE(settle_photos(loop, [&] { return library.has_dialog(); }));
    library.key_dialog(photo_key(XKB_KEY_Return));
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
    browser.open_view_menu(820, 44, true);
    const auto gallery_menu = browser.context_menu().items();
    press(photo_key(XKB_KEY_Escape));
    invoke(gallery_menu, "gallery");
    draw("native-gallery-enabled");
    CHECK(browser.gallery());
    CHECK(browser.grid().gallery());
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
    REQUIRE(settle_photos(
        loop,
        [&] {
            const auto groups = library->client()
                                    ->request({{"op", "duplicates"}})
                                    .value("groups", nlohmann::json::array());
            return !groups.empty() &&
                   groups.front().value("photos", nlohmann::json::array()).size() == 50;
        },
        20s));
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
    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "album-gallery-test");
    Browser browser{loop,  pool,        watcher, rummage::fs::MimeDatabase::load(),
                    [] {}, [](auto) {}, cache};
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
    browser.set_mode(Browser::Mode::grid);
    browser.open_view_menu(820, 44, true);
    const auto gallery_menu = browser.context_menu().items();
    press(photo_key(XKB_KEY_Escape));
    invoke(gallery_menu, "gallery");
    REQUIRE(settle_photos(loop, [&] {
        draw();
        return !browser.loading();
    }));
    draw("native-album-gallery");
    CHECK(browser.gallery());
    CHECK(browser.grid().gallery());
    CHECK(library->rows().size() == 13);
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
            const auto bounds = browser.sidebar().place_icon_rect_for_uri("rummage:photos");
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

TEST_CASE(
    "photo workspace: native Add to album chooser captures twenty-three photos without "
    "navigation") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Album selection";
    for (int i = 0; i < 24; ++i)
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
        settle_photos(loop, [&] { return library->rows().size() == 24 && !library->loading(); }));
    const auto target =
        library->client()->request({{"op", "create_album"}, {"name", "Family album"}});
    REQUIRE(target.value("ok", false));
    const auto other = library->client()->request({{"op", "create_album"},
                                                   {"name", "Other album"},
                                                   {"ids", {library->rows().front().at("id")}}});
    REQUIRE(other.value("ok", false));
    const auto members = [&](const nlohmann::json& id) {
        std::set<PhotoLibrary::Id> ids;
        const auto response = library->client()->request({{"op", "query"}, {"album_id", id}});
        for (const auto& row : response.at("photos"))
            ids.insert(row.at("id").get<PhotoLibrary::Id>());
        return ids;
    };
    const auto original_other = members(other.at("id"));
    browser.set_mode(Browser::Mode::details);
    draw();
    const auto select_twenty_three = [&] {
        press(photo_key(XKB_KEY_F6, {.shift = true}));
        press(photo_key(XKB_KEY_Home));
        for (int i = 0; i < 22; ++i)
            press(photo_key(XKB_KEY_Down, {.shift = true}));
        REQUIRE(library->selected_ids().size() == 23);
    };
    select_twenty_three();
    const auto selected = library->selected_ids().get<std::set<PhotoLibrary::Id>>();
    browser.open_menu(380, 180, false);
    REQUIRE(browser.context_menu().is_open());
    draw("native-add-to-album-context");
    press(photo_key(XKB_KEY_Escape));
    invoke(library->context_menu_items(), "photos:add-to-album");
    REQUIRE(settle_photos(loop, [&] { return browser.command_palette().is_open(); }));
    draw("native-add-to-album-chooser");
    press(photo_key(XKB_KEY_NoSymbol, {}, "Family album"));
    draw("native-add-to-album-search");
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(
        loop, [&] { return members(target.at("id")) == selected && !library->loading(); }));
    CHECK_FALSE(browser.command_palette().is_open());
    CHECK(library->at_all_photos());
    CHECK(library->rows().size() == 24);
    CHECK(library->selected_ids().get<std::set<PhotoLibrary::Id>>() == selected);
    draw("native-add-to-album-preserved-selection");
    CHECK(members(other.at("id")) == original_other);
    select_twenty_three();
    invoke(library->menu_items(), "photos:add-to-album");
    REQUIRE(settle_photos(loop, [&] { return browser.command_palette().is_open(); }));
    const auto items = browser.command_palette().items();
    const auto stale = std::ranges::find(items, "photos:album-choice:" + other.at("id").dump(),
                                         &rummage::views::PaletteItem::id);
    REQUIRE(stale != items.end());
    const auto cancelled_action = stale->action;
    press(photo_key(XKB_KEY_Escape));
    CHECK_FALSE(browser.command_palette().is_open());
    cancelled_action();
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    CHECK(members(other.at("id")) == original_other);
    CHECK(members(target.at("id")) == selected);
    CHECK(library->selected_ids().get<std::set<PhotoLibrary::Id>>() == selected);
    // Cancel before the asynchronous list arrives: it must not reopen a chooser.
    invoke(library->menu_items(), "photos:add-to-album");
    library->cancel_album_chooser();
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    CHECK_FALSE(browser.command_palette().is_open());
    invoke(library->menu_items(), "photos:add-to-album");
    REQUIRE(settle_photos(loop, [&] { return browser.command_palette().is_open(); }));
    press(photo_key(XKB_KEY_NoSymbol, {}, "Create new album"));
    library->folder()->set_cursor(23);
    const auto changed_selection = library->selected_ids();
    REQUIRE(changed_selection.size() == 1);
    press(photo_key(XKB_KEY_Return));
    REQUIRE(library->has_dialog());
    CHECK_FALSE(browser.command_palette().is_open());
    press(photo_key(XKB_KEY_NoSymbol, {}, "Created from chooser"));
    draw("native-add-to-new-album");
    press(photo_key(XKB_KEY_Return));
    nlohmann::json created_id;
    REQUIRE(settle_photos(loop, [&] {
        const auto response = library->client()->request({{"op", "albums"}});
        for (const auto& album : response.at("albums"))
            if (album.value("name", "") == "Created from chooser")
                created_id = album.at("id");
        return !created_id.is_null() && !library->loading();
    }));
    CHECK(members(created_id) == selected);
    CHECK(members(other.at("id")) == original_other);
    CHECK(members(target.at("id")) == selected);
    CHECK(library->at_all_photos());
    CHECK(library->selected_ids() == changed_selection);
    invoke(library->menu_items(), "photos:clear-selection");
    for (const auto& menu : {library->menu_items(), library->context_menu_items()}) {
        const auto item =
            std::ranges::find(menu, "photos:add-to-album", &rummage::views::MenuItem::id);
        REQUIRE(item != menu.end());
        CHECK_FALSE(item->enabled);
    }
    draw("native-add-to-album-complete");
}

TEST_CASE(
    "photo workspace: native sidebar hierarchy opens albums and saved searches without filesystem "
    "paths") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Sidebar pictures";
    for (const auto* name : {"Blue.png", "Green.png", "Red.png"})
        fixture(source / name);
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "sidebar-photo-test");
    Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}, cache};
    const std::vector<rummage::fs::Place> places{
        {.label = "Home", .path = directory.path, .icon = "home"},
        {.label = "Pictures", .path = source, .icon = "photo"}};
    browser.set_places(places);
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
    const auto click = [&](const rummage::ui::Rect& bounds) {
        for (auto kind :
             {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
            browser.pointer({.kind = kind,
                             .x = bounds.x + bounds.width / 2,
                             .y = bounds.y + bounds.height / 2,
                             .button = rummage::ui::PointerButton::left},
                            Browser::Clock::now());
    };
    const auto click_uri = [&](std::string_view uri) {
        draw();
        const auto bounds = browser.sidebar().place_icon_rect_for_uri(uri);
        REQUIRE(bounds);
        click(*bounds);
        REQUIRE(settle_photos(
            loop, [&] { return browser.photo_library() && !browser.photo_library()->loading(); }));
        draw();
        const auto* current = browser.sidebar().current_place();
        REQUIRE(current);
        CHECK(current->uri == uri);
    };
    browser.open(source);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    draw();
    CHECK(browser.sidebar().place_icon_rect_for_uri("rummage:photos"));
    CHECK(browser.sidebar().place_icon_rect_for_uri("rummage:photos/albums"));
    const auto catalog = directory.path / "data/rummage/photos/catalog.sqlite3";
    CHECK_FALSE(std::filesystem::exists(catalog));
    CHECK(browser.photo_library() == nullptr);
    {
        Browser picker{loop, pool, watcher, mime, [] {}, [](auto) {}};
        picker.set_picker_mode(true);
        picker.set_places(places);
        CHECK(std::ranges::none_of(picker.sidebar().sources(), [](const auto& place) {
            return place.uri.starts_with("rummage:photos");
        }));
        CHECK(picker.sidebar().sources().size() == places.size());
        CHECK(picker.photo_library() == nullptr);
    }
    CHECK_FALSE(std::filesystem::exists(catalog));
    click_uri("rummage:photos");
    auto* library = browser.photo_library();
    REQUIRE(library);
    library->add_folder(source);
    press(photo_key(XKB_KEY_Return));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 3 && !library->loading(); }));
    const auto target =
        library->client()->request({{"op", "create_album"}, {"name", "Target album"}});
    REQUIRE(target.value("ok", false));
    library->refresh_navigation();
    browser.set_mode(Browser::Mode::details);
    draw();
    press(photo_key(XKB_KEY_F6, {.shift = true}));
    press(photo_key(XKB_KEY_Home));
    press(photo_key(XKB_KEY_Down, {.shift = true}));
    const auto selected = library->selected_ids();
    REQUIRE(selected.size() == 2);
    invoke(library->menu_items(), "photos:create-album");
    press(photo_key(XKB_KEY_NoSymbol, {}, "Weekend"));
    press(photo_key(XKB_KEY_Return));
    const auto record_id = [&](std::string_view collection, std::string_view name) {
        const auto records =
            library->navigation().value(std::string{collection}, nlohmann::json::array());
        for (const auto& record : records)
            if (record.value("name", "") == name)
                return record.value("id", PhotoLibrary::Id{});
        return PhotoLibrary::Id{};
    };
    REQUIRE(settle_photos(
        loop, [&] { return record_id("albums", "Weekend") > 0 && !library->loading(); }));
    const auto weekend_uri =
        "rummage:photos/albums/" + std::to_string(record_id("albums", "Weekend"));
    const auto target_uri = "rummage:photos/albums/" + target.at("id").dump();
    click_uri("rummage:photos/albums");
    CHECK(library->destination() == PhotoLibrary::Destination::albums);
    CHECK(library->selected_ids() == selected);
    press(photo_key(XKB_KEY_F6));
    press(photo_key(XKB_KEY_Right));
    draw();
    REQUIRE(browser.sidebar().place_icon_rect_for_uri(weekend_uri));
    REQUIRE(browser.sidebar().place_icon_rect_for_uri(target_uri));
    press(photo_key(XKB_KEY_F6, {.shift = true}));
    const auto& album_entries = library->folder()->entries();
    const auto target_row =
        std::ranges::find(album_entries, "Target album", &rummage::fs::Entry::name);
    REQUIRE(target_row != album_entries.end());
    press(photo_key(XKB_KEY_Home));
    for (auto it = album_entries.begin(); it != target_row; ++it)
        press(photo_key(XKB_KEY_Down));
    // Album keys use the native dialogs and never rename/delete the cover original.
    press(photo_key(XKB_KEY_F2));
    REQUIRE(library->has_dialog());
    CHECK(library->copy_selection(false) == "Target album");
    press(photo_key(XKB_KEY_Escape));
    for (const auto modifiers : {rummage::ui::Modifiers{}, rummage::ui::Modifiers{.shift = true}}) {
        press(photo_key(XKB_KEY_Delete, modifiers));
        REQUIRE(library->has_dialog());
        press(photo_key(XKB_KEY_Escape));
    }
    REQUIRE(library->selected_ids() == selected);
    int album_clipboard_offers = 0;
    rummage::views::SystemClipboard album_clipboard{};
    album_clipboard.offer = [&](auto) { ++album_clipboard_offers; };
    browser.set_system_clipboard(std::move(album_clipboard));
    press(photo_key(XKB_KEY_c, {.ctrl = true}));
    press(photo_key(XKB_KEY_x, {.ctrl = true}));
    browser.copy_path();
    CHECK(album_clipboard_offers == 0);
    invoke(library->context_menu_items(), "photos:album-add");
    REQUIRE(settle_photos(loop, [&] {
        const auto response =
            library->client()->request({{"op", "query"}, {"album_id", target.at("id")}});
        return response.value("total", 0) == 2 && !library->loading();
    }));
    CHECK(library->selected_ids() == selected);
    draw("native-sidebar-albums");
    click_uri(weekend_uri);
    CHECK(library->rows().size() == 2);
    CHECK(library->selected_ids() == selected);
    CHECK(library->title().find("Weekend") != std::string::npos);
    draw("native-sidebar-album-collection");
    // Navigate and collapse the hierarchy with the existing sidebar keyboard behavior.
    press(photo_key(XKB_KEY_F6));
    press(photo_key(XKB_KEY_Left));
    press(photo_key(XKB_KEY_Left));
    draw();
    CHECK_FALSE(browser.sidebar().place_icon_rect_for_uri(weekend_uri));
    REQUIRE(browser.sidebar().current_place());
    CHECK(browser.sidebar().current_place()->uri == "rummage:photos/albums");
    press(photo_key(XKB_KEY_Right));
    press(photo_key(XKB_KEY_Right));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 2 && !library->loading(); }));
    draw();
    REQUIRE(browser.sidebar().current_place());
    const auto* active_album = browser.sidebar().current_place();
    CHECK((active_album && active_album->uri.starts_with("rummage:photos/albums/")));
    click_uri("rummage:photos");
    CHECK(library->at_all_photos());
    CHECK(library->rows().size() == 3);
    library->set_query("Blue");
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 1 && !library->loading(); }));
    invoke(library->menu_items(), "photos:save-search");
    press(photo_key(XKB_KEY_NoSymbol, {}, "Blue sky"));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(
        loop, [&] { return record_id("filters", "Blue sky") > 0 && !library->loading(); }));
    const auto search_uri =
        "rummage:photos/searches/" + std::to_string(record_id("filters", "Blue sky"));
    click_uri("rummage:photos");
    click_uri("rummage:photos/searches");
    CHECK(library->destination() == PhotoLibrary::Destination::filters);
    press(photo_key(XKB_KEY_F6));
    press(photo_key(XKB_KEY_Right));
    draw();
    REQUIRE(browser.sidebar().place_icon_rect_for_uri(search_uri));
    press(photo_key(XKB_KEY_F6, {.shift = true}));
    click_uri(search_uri);
    REQUIRE(library->rows().size() == 1);
    CHECK(library->rows().front().value("filename", "") == "Blue.png");
    CHECK(library->query_text() == "Blue");
    draw("native-sidebar-saved-search");
    click_uri("rummage:photos");
    for (const auto& [uri, destination] :
         std::array{std::pair{"rummage:photos/sources", PhotoLibrary::Destination::sources},
                    std::pair{"rummage:photos/review", PhotoLibrary::Destination::review},
                    std::pair{"rummage:photos/activity", PhotoLibrary::Destination::activity}}) {
        click_uri(uri);
        CHECK(library->destination() == destination);
        CHECK(library->folder()->is_collection());
        CHECK(library->folder()->path().empty());
        for (const auto& entry : library->folder()->entries()) {
            CHECK(entry.location.empty());
            CHECK_FALSE(entry.is_directory());
        }
    }
    click_uri("rummage:photos");
    press(photo_key(XKB_KEY_F6));
    press(photo_key(XKB_KEY_Left));
    draw();
    CHECK_FALSE(browser.sidebar().place_icon_rect_for_uri("rummage:photos/albums"));
    CHECK(browser.sidebar().place_icon_rect_for_uri("rummage:photos"));
    press(photo_key(XKB_KEY_Right));
    draw("native-sidebar-expanded");
    CHECK(browser.sidebar().place_icon_rect_for_uri(weekend_uri));
    CHECK(browser.sidebar().place_icon_rect_for_uri(search_uri));
    for (const auto& place : browser.sidebar().places())
        if (place.uri.starts_with("rummage:photos"))
            CHECK(place.path.empty());
    press(photo_key(XKB_KEY_F6, {.shift = true}));
    const auto home = browser.sidebar().place_icon_rect(directory.path);
    REQUIRE(home);
    click(*home);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    CHECK_FALSE(browser.showing_photos());
    CHECK(browser.picker_folder()->path() == directory.path);
    draw();
    const auto album_bounds = browser.sidebar().place_icon_rect_for_uri(weekend_uri);
    REQUIRE(album_bounds);
    for (auto kind :
         {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
        browser.pointer({.kind = kind,
                         .x = album_bounds->x + album_bounds->width / 2,
                         .y = album_bounds->y + album_bounds->height / 2,
                         .button = rummage::ui::PointerButton::right},
                        Browser::Clock::now());
    const auto sidebar_actions = browser.context_menu().items();
    press(photo_key(XKB_KEY_Escape));
    invoke(sidebar_actions, "photos:rename-album");
    REQUIRE(browser.showing_photos());
    REQUIRE(library->has_dialog());
    CHECK(library->copy_selection(false) == "Weekend");
    press(photo_key(XKB_KEY_Escape));
    click_uri("rummage:photos");
    CHECK(library->at_all_photos());
    CHECK(library->rows().size() == 3);
}

TEST_CASE("photo workspace: native scrolling appends every collection beyond two hundred photos") {
    rummage::ui::Offscreen offscreen{1100, 820};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Continuous pictures";
    constexpr std::size_t count = 450;
    for (std::size_t i = 0; i < count; ++i)
        fixture(source / ("photo-" + std::to_string(1000 + i) + ".png"));
    rummage::photos::Client catalog;
    REQUIRE(catalog.available());
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    nlohmann::json all;
    for (int batch = 0; batch < 16; ++batch) {
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 256}}).value("ok", false));
        all = catalog.request({{"op", "query"}, {"limit", 1000}});
        if (all.value("total", std::size_t{}) == count)
            break;
    }
    REQUIRE(all.at("photos").size() == count);
    // Discovery is real; stop background metadata cadence from extending viewport-test timing.
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    nlohmann::json ids = nlohmann::json::array();
    for (const auto& row : all.at("photos"))
        ids.push_back(row.at("id"));
    const auto album = catalog.request({{"op", "create_album"}, {"name", "All 450"}, {"ids", ids}});
    REQUIRE(album.value("ok", false));
    REQUIRE(
        catalog
            .request(
                {{"op", "save_filter"}, {"name", "Every photo"}, {"filter", {{"text", "photo-"}}}})
            .value("ok", false));
    const auto searches = catalog.request({{"op", "filters"}}).at("filters");
    REQUIRE(searches.size() == 1);
    const auto saved_search = searches.front();
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    for (const auto mode : {Browser::Mode::grid, Browser::Mode::details, Browser::Mode::columns}) {
        for (const auto collection : {"library", "album", "saved search"}) {
            INFO("Native mode " << static_cast<int>(mode) << ", " << collection);
            Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}};
            browser.set_places({{.label = "Home", .path = directory.path, .icon = "home"},
                                {.label = "Pictures", .path = source, .icon = "photo"}});
            browser.open(directory.path);
            browser.open_photo_library();
            auto* library = browser.photo_library();
            REQUIRE(library);
            REQUIRE(settle_photos(
                loop, [&] { return library->rows().size() == 200 && !library->loading(); }));
            if (std::string_view{collection} == "album")
                library->open_album(album.at("id").get<PhotoLibrary::Id>(), "All 450");
            else if (std::string_view{collection} == "saved search")
                library->open_saved_search(saved_search);
            REQUIRE(settle_photos(
                loop, [&] { return library->rows().size() == 200 && !library->loading(); }));
            browser.set_mode(mode);
            const auto draw = [&] {
                const rummage::theme::Palette palette;
                renderer.begin_frame(1100, 820, 1, palette.surface);
                browser.draw(renderer, {0, 0, 1100, 820}, palette, icons, Browser::Clock::now());
                renderer.end_frame();
            };
            REQUIRE(settle_photos(loop, [&] {
                draw();
                return !browser.animating();
            }));
            REQUIRE(browser.mode() == mode);
            REQUIRE(browser.key(photo_key(XKB_KEY_Home), Browser::Clock::now()));
            REQUIRE(
                browser.key(photo_key(mode == Browser::Mode::grid ? XKB_KEY_Right : XKB_KEY_Down,
                                      {.shift = true}),
                            Browser::Clock::now()));
            const auto selected = library->selected_ids();
            REQUIRE(selected.size() == 2);
            draw();
            const auto scroller = [&]() -> rummage::ui::Scroller& {
                if (mode == Browser::Mode::grid)
                    return browser.grid().scroller();
                if (mode == Browser::Mode::details)
                    return browser.details().scroller();
                return browser.columns().column(0).scroller();
            };
            std::vector<std::string> prefix;
            for (const auto& entry : library->folder()->entries())
                prefix.push_back(entry.key());
            for (const auto& menu : {library->menu_items(), library->context_menu_items()})
                CHECK(std::ranges::none_of(menu, [](const auto& item) {
                    return item.id == "photos:previous" || item.id == "photos:next";
                }));
            CHECK(library->status().find("Page ") == std::string::npos);
            for (int append = 0; append < 3 && library->rows().size() < count; ++append) {
                const auto loaded = library->rows().size();
                REQUIRE(scroller().max_offset() > 0);
                browser.pointer({.kind = rummage::ui::PointerEvent::Kind::scroll,
                                 .x = 350,
                                 .y = 350,
                                 .scroll_y = 100000},
                                Browser::Clock::now());
                const auto before_append = scroller().offset();
                REQUIRE(before_append > 0);
                draw();
                REQUIRE(settle_photos(loop, [&] {
                    draw();
                    return library->rows().size() > loaded && !library->loading();
                }));
                CHECK(scroller().offset() >= before_append - 1);
                CHECK(library->selected_ids() == selected);
                CHECK(library->folder()->total_count() == count);
                REQUIRE(library->folder()->entries().size() >= prefix.size());
                for (std::size_t i = 0; i < prefix.size(); ++i)
                    CHECK(library->folder()->entries()[i].key() == prefix[i]);
            }
            REQUIRE(library->rows().size() == count);
            std::set<std::string> unique_keys;
            for (const auto& entry : library->folder()->entries())
                unique_keys.insert(entry.key());
            CHECK(unique_keys.size() == count);
            CHECK(library->folder()->entries().back().name == "photo-1449.png");
            const auto before_refresh = scroller().offset();
            library->refresh();
            REQUIRE(settle_photos(loop, [&] {
                draw();
                return !library->loading();
            }));
            CHECK(library->rows().size() == count);
            CHECK(library->selected_ids() == selected);
            CHECK(scroller().offset() >= before_refresh - 1);
            if (mode == Browser::Mode::grid && std::string_view{collection} == "library") {
                capture("native-continuous-library-450", 1100, 820);
                library->folder()->set_cursor(count - 1);
                draw();
                const auto cursor_key = library->folder()->entries().back().key();
                const auto saved = browser.state();
                Browser reopened{loop, pool, watcher, mime, [] {}, [](auto) {}};
                reopened.apply(saved);
                REQUIRE(reopened.restore_session(saved));
                REQUIRE(reopened.photo_library());
                // Repeated same-scope invalidations must coalesce, not keep cancelling the query.
                const auto churn =
                    loop.add_timer(1ms, 1ms, [&] { reopened.photo_library()->refresh(); });
                const bool restored = settle_photos(
                    loop, [&] { return reopened.photo_library()->rows().size() == count; });
                loop.remove(churn);
                REQUIRE(restored);
                REQUIRE(settle_photos(loop, [&] { return !reopened.photo_library()->loading(); }));
                const auto* cursor = reopened.photo_library()->folder()->cursor_entry();
                REQUIRE(cursor);
                if (cursor)
                    CHECK(cursor->key() == cursor_key);
                REQUIRE(settle_photos(loop, [&] {
                    const rummage::theme::Palette palette;
                    renderer.begin_frame(1100, 820, 1, palette.surface);
                    reopened.draw(renderer, {0, 0, 1100, 820}, palette, icons,
                                  Browser::Clock::now());
                    renderer.end_frame();
                    return !reopened.animating();
                }));
                CHECK(reopened.grid().scroller().offset() > 0);
                CHECK(reopened.photo_library()->rows().size() == count);
                capture("native-continuous-restored-450", 1100, 820);
            }
            if (mode == Browser::Mode::details && std::string_view{collection} == "album")
                capture("native-continuous-album-450", 1100, 820);
            if (mode == Browser::Mode::columns && std::string_view{collection} == "saved search")
                capture("native-continuous-search-450", 1100, 820);
        }
    }
    // Queue an append, then replace the query before the event loop can deliver its result.
    Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}};
    browser.open(directory.path);
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 200 && !library->loading(); }));
    library->folder()->notify_visible_range(180, 200);
    library->set_query("photo-1000.png");
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 1 && !library->loading(); }));
    library->refresh();
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    REQUIRE(library->rows().size() == 1);
    CHECK(library->rows().front().value("filename", "") == "photo-1000.png");
    CHECK(library->folder()->total_count() == 1);
}

TEST_CASE(
    "photo workspace: multi-batch refresh keeps loaded selection and replaces queued queries") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Many pictures";
    constexpr std::size_t count = 1100;
    for (std::size_t i = 0; i < count; ++i)
        fixture(source / ("photo-" + std::to_string(1000 + i) + ".png"));
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    for (int batch = 0; batch < 32; ++batch) {
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 256}}).value("ok", false));
        if (catalog.request({{"op", "query"}, {"limit", 1}}).value("total", std::size_t{}) == count)
            break;
    }
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    PhotoLibrary library{loop, pool, watcher, mime, [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    for (int page = 0; page < 8 && library.rows().size() < count; ++page) {
        library.folder()->notify_visible_range(0, library.rows().size());
        REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    }
    REQUIRE(library.rows().size() == count);
    library.folder()->set_cursor(count - 1);
    const auto selection = library.selected_ids();
    const auto cursor = library.folder()->cursor_entry()->key();
    const auto original = library.rows();
    for (int refresh = 0; refresh < 3; ++refresh) {
        library.refresh();
        library.refresh();  // coalesced while the complete prefix is in flight
        REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
        CHECK(library.rows() == original);
        CHECK(library.selected_ids() == selection);
        REQUIRE(library.folder()->cursor_entry());
        CHECK(library.folder()->cursor_entry()->key() == cursor);
    }
    std::promise<void> started, release;
    auto ready = started.get_future();
    auto gate = release.get_future().share();
    pool.catalog_lane().submit(
        rummage::core::Priority::visible,
        [&started, gate](const rummage::core::CancelToken&) {
            started.set_value();
            gate.wait();
            return 0;
        },
        [](int) {});
    ready.wait();
    library.refresh();
    library.set_query("photo-1000.png");
    library.set_query("photo-2099.png");
    release.set_value();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.rows().size() == 1);
    CHECK(library.rows().front().value("filename", "") == "photo-2099.png");
    CHECK(library.selected_ids().empty());
    CHECK(library.folder()->total_count() == 1);
}

TEST_CASE("photo workspace: changed originals preview freshly without accepting catalog identity") {
    rummage::ui::Offscreen offscreen{800, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv default_cache{"XDG_CACHE_HOME",
                                                 (directory.path / "default-cache").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Photos";
    const auto original = source / "changed.png";
    fixture(original);
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    const auto indexed = catalog.request({{"op", "query"}}).at("photos");
    REQUIRE(indexed.size() == 1);
    const auto id = indexed.front().at("id");
    const auto signature = indexed.front().at("signature");
    REQUIRE(catalog
                .request({{"op", "edit"},
                          {"ids", {id}},
                          {"description", "Keep my annotation"},
                          {"rating", 4},
                          {"add_tags", {"family"}}})
                .value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    // Replace the actual original with another valid image. Trailing bytes change its size as
    // well as its inode, so this is a real identity mismatch even on a stable local filesystem.
    const auto replacement = directory.path / "replacement.png";
    fixture(replacement);
    std::ofstream{replacement, std::ios::app | std::ios::binary} << "replacement";
    std::filesystem::rename(replacement, original);
    const auto stale = catalog.request({{"op", "metadata"}, {"id", id}, {"signature", signature}});
    REQUIRE(stale.value("stale", false));
    const auto before = catalog.request({{"op", "query"}}).at("photos").front();
    REQUIRE(before.value("availability", "") == "changed");

    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}};
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 1 && !library->loading(); }));
    const auto folder = library->folder();
    folder->set_cursor(0);
    REQUIRE(folder->cursor_entry());
    CHECK_FALSE(folder->cursor_entry()->available);
    CHECK(folder->cursor_entry()->preview_current_location);
    CHECK(folder->cursor_entry()->can_preview());
    CHECK(folder->selected_paths() == std::vector<std::filesystem::path>{original});
    const auto selected = library->selected_ids();
    const auto properties = library->properties(true);
    CHECK_FALSE(properties.available);
    CHECK(std::ranges::any_of(properties.rows,
                              [](const auto& row) { return row.first == "Preview"; }));
    int offers = 0;
    rummage::views::SystemClipboard clipboard{};
    clipboard.offer = [&](auto) { ++offers; };
    browser.set_system_clipboard(std::move(clipboard));
    browser.take_selection(false);
    browser.take_selection(true);
    CHECK(offers == 0);

    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "fresh-cache", "changed-preview");
    REQUIRE_FALSE(std::filesystem::exists(cache->root()));
    rummage::views::Thumbnails thumbnails{loop, pool, cache, [] {}};
    rummage::views::GridView grid{loop, pool, watcher, mime, [] {}, [](auto) {}, &thumbnails};
    rummage::views::InfoPane pane{loop, pool, &thumbnails, [] {}};
    rummage::views::PropertiesDialog dialog{pool, mime, [] {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    std::function<void()> paint;
    std::string frame;
    SUBCASE("Grid reads the changed location") {
        grid.set_info_visible(false);
        grid.show(folder);
        frame = "native-changed-preview-grid";
        paint = [&] { grid.draw(renderer, {0, 0, 800, 700}, {}, icons, Browser::Clock::now()); };
    }
    SUBCASE("InfoPane reads the changed location") {
        pane.set_photo_properties(properties);
        frame = "native-changed-preview-info";
        paint = [&] {
            pane.draw(renderer, {0, 0, 350, 700}, {}, icons, *folder, Browser::Clock::now());
        };
    }
    SUBCASE("Properties reads the changed location") {
        dialog.open_photo(properties, {});
        frame = "native-changed-preview-properties";
        paint = [&] { dialog.draw(renderer, {0, 0, 800, 700}, {}, icons, &thumbnails); };
    }
    REQUIRE(static_cast<bool>(paint));
    const auto draw = [&] {
        renderer.begin_frame(800, 700, 1, {});
        paint();
        thumbnails.end_frame();
        renderer.end_frame();
    };
    REQUIRE(settle_photos(loop, [&] {
        draw();
        return thumbnails.texture_bytes() > 0 && !thumbnails.loading();
    }));
    capture(frame, 800, 700);
    CHECK(std::filesystem::exists(cache->root()));
    CHECK(library->selected_ids() == selected);
    CHECK_FALSE(folder->cursor_entry()->available);
    CHECK(folder->selected_paths() == std::vector<std::filesystem::path>{original});
    const auto after = catalog.request({{"op", "query"}}).at("photos").front();
    for (const auto* key : {"id", "signature", "availability", "description", "rating", "tags"})
        CHECK(after.at(key) == before.at(key));
}

TEST_CASE("photo workspace: metadata patches preserve loaded storage and focused annotations") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    constexpr std::size_t count = 24;
    for (std::size_t index = 0; index < count; ++index)
        fixture(source / (std::to_string(100 + index) + ".png"));
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 256}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "query"}}).at("photos").size() == count);
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    PhotoLibrary library{loop, pool, watcher, mime, [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] {
        // Initial open publishes once; adopted watches reconcile once more. Neither is
        // metadata-only work, so wait for both publications while background work is paused.
        return !library.loading() && library.rows().size() == count &&
               library.folder()->revision() >= 2;
    }));
    const auto progress = catalog.request({{"op", "progress"}}).at("jobs").front();
    CHECK(progress.value("pending_directories", -1) == 0);
    REQUIRE(progress.value("metadata_pending", 0) > 0);
    const auto folder = library.folder();
    folder->set_cursor(count - 1);
    const auto id = library.properties(true).ids.front();
    invoke(library.menu_items(), "photos:pause");
    REQUIRE(settle_photos(loop, [&] {
        return !library.loading() &&
               library.status().find("Background work resumed.") != std::string::npos;
    }));
    // Resume is an explicit action that refreshes once. Observe only subsequent metadata work.
    const auto* storage = folder->entries().data();
    const auto selected = library.selected_ids();
    bool replaced_storage = false;
    std::string replacement_status;
    const auto observed = loop.add_timer(5ms, 5ms, [&] {
        if (folder->entries().data() != storage) {
            replaced_storage = true;
            if (replacement_status.empty())
                replacement_status = library.status();
        }
    });
    REQUIRE(settle_photos(
        loop,
        [&] {
            return library.rows().size() == count &&
                   std::ranges::all_of(library.rows(), [](const auto& row) {
                       return row.value("width", 0) > 0 && row.value("height", 0) > 0;
                   });
        },
        12s));
    loop.remove(observed);
    INFO(replacement_status);
    CHECK_FALSE(replaced_storage);
    CHECK(library.selected_ids() == selected);
    CHECK(library.properties(true).ids == std::vector<PhotoLibrary::Id>{id});
    CHECK(std::ranges::any_of(library.properties(true).rows, [](const auto& row) {
        return row.first == "Dimensions" && !row.second.empty();
    }));
    // A metadata patch must update the retained selection snapshot as well. Moving to an
    // empty album forces bulk properties to read that snapshot rather than the loaded rows.
    const auto empty_album = catalog.request({{"op", "create_album"}, {"name", "Snapshot check"}});
    REQUIRE(empty_album.value("ok", false));
    library.open_album(empty_album.at("id").get<PhotoLibrary::Id>(), "Snapshot check");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.rows().empty());
    CHECK(library.selected_ids() == selected);
    CHECK(std::ranges::any_of(library.properties().rows, [](const auto& row) {
        return row.first == "Dimensions" && !row.second.empty();
    }));
    invoke(library.menu_items(), "photos:clear-selection");
    CHECK(library.selected_ids().empty());
    CHECK(library.properties().ids.empty());
    // No cursor means no focused inspector, including a large rubber-band selection.
    folder->set_cursor(std::nullopt);
    folder->select_all();
    folder->set_cursor(std::nullopt);
    CHECK(library.properties(true).ids.empty());
}

TEST_CASE(
    "photo workspace: retained album associations remain explicit and untranslated user names "
    "survive") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "original.png");
    rummage::photos::Client catalog;
    const auto added = catalog.request({{"op", "add_source"}, {"path", source.string()}});
    REQUIRE(added.value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    const auto original = catalog.request({{"op", "query"}}).at("photos").front();
    const auto album =
        catalog.request({{"op", "create_album"}, {"name", "paused"}, {"ids", {original.at("id")}}});
    REQUIRE(album.value("ok", false));
    REQUIRE(catalog.request({{"op", "remove_source"}, {"source_id", added.at("source").at("id")}})
                .value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    PhotoLibrary library{loop, pool, watcher, mime, [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().empty());
    library.open_album(album.at("id").get<PhotoLibrary::Id>(), "paused");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().empty());
    invoke(library.menu_items(), "photos:retained");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.rows().size() == 1);
    CHECK(library.rows().front().at("id") == original.at("id"));
    CHECK(library.rows().front().value("availability", "") == "inactive");
    CHECK_FALSE(library.folder()->entries().front().available);
    CHECK_FALSE(library.folder()->entries().front().can_preview());
    CHECK(library.status().find("Retained associations") != std::string::npos);
    CHECK(std::filesystem::exists(source / "original.png"));
    library.show(PhotoLibrary::Destination::albums);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.folder()->set_cursor(0);

    struct ResetLanguage {
        ~ResetLanguage() { rummage::i18n::set_language("en"); }
    } reset_language;

    rummage::i18n::set_language("de");
    const auto props = library.properties(true);
    CHECK(props.title == "paused");
    CHECK(std::ranges::any_of(props.rows, [](const auto& row) { return row.second == "paused"; }));
    CHECK(std::ranges::any_of(props.rows, [](const auto& row) { return row.second == "1"; }));
    const auto menu = library.menu_items();
    const auto retained = std::ranges::find(menu, "photos:retained", &rummage::views::MenuItem::id);
    REQUIRE(retained != menu.end());
    CHECK(retained->label != "Review retained photo associations");
    CHECK_FALSE(retained->label.empty());
    rummage::i18n::set_language("en");
    library.show_all_photos();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().empty());
}

TEST_CASE("photo workspace: duplicate review exposes every group beyond the first two hundred") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    constexpr std::size_t groups = 202;
    for (std::size_t group = 0; group < groups; ++group)
        for (const auto suffix : {"a", "b"}) {
            const auto path = source / (std::to_string(group) + suffix + ".png");
            fixture(path);
            std::ofstream{path, std::ios::binary | std::ios::app} << "group:" << group;
        }
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    const auto drain = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        bool idle = false;
        do {
            const auto work = catalog.request({{"op", "work"}, {"budget", 256}});
            REQUIRE(work.value("ok", false));
            idle = !work.value("worked", false) && !work.value("busy", false);
        } while (!idle && std::chrono::steady_clock::now() < deadline);
        INFO(catalog.request({{"op", "jobs"}}).dump());
        REQUIRE(idle);
    };
    drain();
    REQUIRE(catalog.request({{"op", "query"}, {"limit", 1}}).at("total") == groups * 2);
    REQUIRE(catalog.request({{"op", "find_duplicates"}}).value("ok", false));
    drain();
    REQUIRE(catalog.request({{"op", "duplicates"}}).at("total") == groups);
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    PhotoLibrary library{loop, pool, watcher, mime, [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.show(PhotoLibrary::Destination::review);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.folder()->entries().size() == 200);
    CHECK(library.status().find("Groups 1–200 of 202") != std::string::npos);
    const auto first_key = library.folder()->entries().front().key();
    invoke(library.menu_items(), "photos:duplicate-next");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.folder()->entries().size() == 2);
    CHECK(library.status().find("Groups 201–202 of 202") != std::string::npos);
    CHECK(library.folder()->entries().front().key() != first_key);
    const auto menu = library.menu_items();
    const auto next =
        std::ranges::find(menu, "photos:duplicate-next", &rummage::views::MenuItem::id);
    REQUIRE(next != menu.end());
    CHECK_FALSE(next->enabled);
    invoke(library.menu_items(), "photos:duplicate-previous");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.folder()->entries().front().key() == first_key);
}

TEST_CASE("photo workspace: album management preserves originals and renders chosen covers") {
    rummage::ui::Offscreen offscreen{800, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "First.png");
    fixture(source / "Second.png");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    library.add_folder(source);
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return library.rows().size() == 2 && !library.loading(); }));
    const auto first = library.rows()[0].at("id");
    const auto second = library.rows()[1].at("id");
    const auto album = library.client()->request(
        {{"op", "create_album"}, {"name", "Original album"}, {"ids", {first, second}}});
    REQUIRE(album.value("ok", false));
    const auto id = album.at("id").get<PhotoLibrary::Id>();
    library.open_album(id, "Original album");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    invoke(library.context_menu_items(), "photos:rename-album");
    REQUIRE(library.has_dialog());
    CHECK(library.copy_selection(false) == "Original album");
    library.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, "Renamed album"));
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(
        loop, [&] { return !library.loading() && library.title() == "Album: Renamed album"; }));
    REQUIRE(library.navigation().at("albums").size() == 1);
    CHECK(library.navigation().at("albums")[0].at("name") == "Renamed album");
    library.folder()->set_cursor(1);
    const auto cover = library.focused_photo();
    REQUIRE(!cover.empty());
    invoke(library.context_menu_items(), "photos:set-album-cover");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.navigation().at("albums")[0].at("cover_id") == cover.at("id"));
    library.show(PhotoLibrary::Destination::albums);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.folder()->entries().size() == 1);
    const auto entry = library.folder()->entries().front();
    CHECK(entry.name == "Renamed album");
    CHECK(entry.key() == "album:" + std::to_string(id));
    CHECK(entry.location == cover.at("path").get<std::string>());
    CHECK(entry.modified_ns == cover.at("mtime_ns").get<std::int64_t>());
    CHECK(library.folder()->mime().name(entry.mime) == "image/png");
    // Exercise the actual grid path with an empty cache, not just the catalog cover fields.
    const auto cache =
        std::make_shared<rummage::thumbs::Cache>(directory.path / "cache", "album-cover-test");
    rummage::views::Thumbnails thumbnails{loop, pool, cache, [] {}};
    rummage::views::GridView grid{loop,  pool,        watcher,    rummage::fs::MimeDatabase::load(),
                                  [] {}, [](auto) {}, &thumbnails};
    grid.set_info_visible(false);
    grid.show(library.folder());
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    REQUIRE(settle_photos(loop, [&] {
        renderer.begin_frame(800, 700, 1, {});
        grid.draw(renderer, {0, 0, 800, 700}, {}, icons, Browser::Clock::now());
        thumbnails.end_frame();
        renderer.end_frame();
        return thumbnails.texture_bytes() > 0 && !thumbnails.loading();
    }));
    capture("native-album-chosen-cover", 800, 700);
    library.folder()->set_cursor(0);
    invoke(library.context_menu_items(), "photos:reset-album-cover");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.navigation().at("albums")[0].at("custom_cover_id") == 0);
    library.folder()->set_cursor(0);
    CHECK(library.activate());
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().size() == 2);
    invoke(library.menu_items(), "photos:delete-album");
    REQUIRE(library.has_dialog());
    library.key_dialog(photo_key(XKB_KEY_Escape));
    CHECK(library.client()->request({{"op", "albums"}}).at("albums").size() == 1);
    invoke(library.menu_items(), "photos:delete-album");
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        return !library.loading() && library.destination() == PhotoLibrary::Destination::albums;
    }));
    CHECK(library.folder()->entries().empty());
    CHECK(library.navigation().at("albums").empty());
    library.show_all_photos();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().size() == 2);
    CHECK(std::filesystem::exists(source / "First.png"));
    CHECK(std::filesystem::exists(source / "Second.png"));
}
#endif
