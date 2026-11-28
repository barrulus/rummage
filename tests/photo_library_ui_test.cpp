#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <sqlite3.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/i18n.hpp"
#include "rummage/picture/wuffs.hpp"
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

const rummage::views::MenuItem* find_menu_item(const std::vector<rummage::views::MenuItem>& items,
                                               std::string_view id) {
    for (const auto& item : items) {
        if (item.id == id)
            return &item;
        if (const auto* child = find_menu_item(item.children, id))
            return child;
    }
    return nullptr;
}

void invoke(std::vector<rummage::views::MenuItem> items, std::string_view id) {
    INFO("Menu action: " << id);
    const auto* found = find_menu_item(items, id);
    REQUIRE(found);
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
    CHECK(browser.mode() == Browser::Mode::grid);
    for (const auto destination :
         {PhotoLibrary::Destination::albums, PhotoLibrary::Destination::sources,
          PhotoLibrary::Destination::filters, PhotoLibrary::Destination::review,
          PhotoLibrary::Destination::activity}) {
        browser.photo_library()->show(destination);
        CHECK(browser.mode() == Browser::Mode::grid);
        browser.photo_library()->show_all_photos();
        CHECK(browser.mode() == Browser::Mode::grid);
    }
    browser.set_mode(Browser::Mode::details);
    browser.photo_library()->show(PhotoLibrary::Destination::albums);
    CHECK(browser.mode() == Browser::Mode::details);
    browser.set_mode(Browser::Mode::grid);
    browser.photo_library()->show_all_photos();
    browser.photo_library()->show(PhotoLibrary::Destination::albums);
    CHECK(browser.mode() == Browser::Mode::grid);
    browser.set_photos_enabled(false);
    CHECK_FALSE(browser.showing_photos());
    CHECK(browser.photo_library() == nullptr);
    CHECK(std::filesystem::exists(catalog));
}

TEST_CASE("photo workspace: switching from native search preserves both collection identities") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "first" / "same.png");
    fixture(source / "second" / "same.png");
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    browser.open(source);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    browser.open_search_results(source, "*.png", false);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    const auto search_tab = browser.active_tab();
    const auto search_folder = browser.picker_folder();
    REQUIRE(search_folder->is_search());
    CHECK_FALSE(search_folder->is_external_collection());
    REQUIRE(search_folder->entries().size() == 2);
    search_folder->set_cursor(1);
    const auto selected_search = search_folder->selected_names();
    const auto selected_path = search_folder->path_of(*search_folder->cursor_entry());
    CHECK_FALSE(browser.has_overlay());

    browser.open_photo_library();
    REQUIRE(browser.showing_photos());
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading() && library->rows().size() == 2; }));
    const auto photo_tab = browser.active_tab();
    const auto photo_folder = library->folder();
    REQUIRE(photo_folder->is_external_collection());
    CHECK_FALSE(photo_folder->is_search());
    REQUIRE(photo_folder->entries().size() == 2);
    CHECK(photo_folder->entries()[0].name == photo_folder->entries()[1].name);
    CHECK(photo_folder->entries()[0].key() != photo_folder->entries()[1].key());
    photo_folder->set_cursor(1);
    const auto selected_photo = library->selected_ids();
    REQUIRE(selected_photo.size() == 1);

    // The former global search-close path discarded this tab's live search on opening Photos.
    browser.select_tab(search_tab);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    CHECK(browser.searching_results());
    CHECK(browser.picker_folder() == search_folder);
    CHECK(browser.name_filter() == "*.png");
    CHECK(search_folder->selected_names() == selected_search);
    REQUIRE(search_folder->cursor_entry());
    CHECK(search_folder->path_of(*search_folder->cursor_entry()) == selected_path);
    browser.select_tab(photo_tab);
    REQUIRE(browser.showing_photos());
    CHECK(browser.picker_folder() == photo_folder);
    CHECK(library->selected_ids() == selected_photo);

    // An implicit search focuses Photo Library's native query; an explicit filesystem scope
    // gets a separate ordinary search tab without converting or clearing either collection.
    const auto tabs = browser.tab_count();
    browser.open_search_results();
    CHECK(browser.showing_photos());
    CHECK(browser.filtering());
    CHECK(browser.tab_count() == tabs);
    CHECK_FALSE(browser.searching_results());
    browser.open_search_results(source, "name:same.png", false);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    CHECK_FALSE(browser.showing_photos());
    CHECK(browser.searching_results());
    CHECK(browser.tab_count() == tabs + 1);
    REQUIRE(browser.picker_folder()->entries().size() == 2);
    browser.select_tab(photo_tab);
    REQUIRE(browser.showing_photos());
    CHECK(browser.picker_folder() == photo_folder);
    CHECK(library->selected_ids() == selected_photo);
    browser.select_tab(search_tab);
    CHECK(browser.searching_results());
    CHECK(browser.picker_folder() == search_folder);
    CHECK(search_folder->selected_names() == selected_search);
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
    invoke(library->menu_items(), "photos:edit");
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
    CHECK(browser.mode() == Browser::Mode::grid);
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
    REQUIRE(settle_photos(loop, [&] { return library->has_dialog(); }));
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
    library->show_all_photos();
    REQUIRE(
        settle_photos(loop, [&] { return library->rows().size() == 14 && !library->loading(); }));
    // Leaving the album has already cleared its selection; there is nothing left to clear.
    REQUIRE(library->selected_ids().empty());
    const auto actions = library->menu_items();
    const auto* clear = find_menu_item(actions, "photos:clear-selection");
    REQUIRE(clear);
    CHECK_FALSE(clear->enabled);
    invoke(library->menu_items(), "photos:create-album");
    REQUIRE(settle_photos(loop, [&] { return library->has_dialog(); }));
    press(photo_key(XKB_KEY_NoSymbol, {}, "Empty album"));
    press(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] {
        const auto albums = library->client()->request({{"op", "albums"}}).at("albums");
        return std::ranges::any_of(albums, [](const auto& album) {
            return album.value("name", "") == "Empty album" && album.value("count", -1) == 0;
        });
    }));
    for (const auto route : {"sidebar", "back", "parent", "controller"}) {
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
            library->show_all_photos();
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
    REQUIRE(settle_photos(loop, [&] { return library->has_dialog(); }));
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
    const auto toolbar = library->menu_items();
    const auto* add_to_album = find_menu_item(toolbar, "photos:add-to-album");
    REQUIRE(add_to_album);
    CHECK_FALSE(add_to_album->enabled);
    const auto empty_context = library->context_menu_items();
    CHECK_FALSE(find_menu_item(empty_context, "photos:add-to-album"));
    REQUIRE(empty_context.size() == 1);
    CHECK(empty_context[0].id == "photos:select-page");
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
    const auto cold_albums_group =
        browser.sidebar().place_icon_rect_for_uri("rummage:photos/albums");
    REQUIRE(cold_albums_group);
    for (auto kind :
         {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
        browser.pointer({.kind = kind,
                         .x = cold_albums_group->x + cold_albums_group->width / 2,
                         .y = cold_albums_group->y + cold_albums_group->height / 2,
                         .button = rummage::ui::PointerButton::right},
                        Browser::Clock::now());
    const auto cold_create_actions = browser.context_menu().items();
    press(photo_key(XKB_KEY_Escape));
    invoke(cold_create_actions, "photos:create-album");
    REQUIRE(settle_photos(
        loop, [&] { return browser.photo_library() && browser.photo_library()->has_dialog(); }));
    CHECK(browser.showing_photos());
    CHECK(browser.photo_library()->destination() == PhotoLibrary::Destination::albums);
    press(photo_key(XKB_KEY_Escape));
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
    REQUIRE(settle_photos(loop, [&] { return library->has_dialog(); }));
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
    const auto albums_group = browser.sidebar().place_icon_rect_for_uri("rummage:photos/albums");
    REQUIRE(albums_group);
    for (auto kind :
         {rummage::ui::PointerEvent::Kind::press, rummage::ui::PointerEvent::Kind::release})
        browser.pointer({.kind = kind,
                         .x = albums_group->x + albums_group->width / 2,
                         .y = albums_group->y + albums_group->height / 2,
                         .button = rummage::ui::PointerButton::right},
                        Browser::Clock::now());
    const auto group_actions = browser.context_menu().items();
    press(photo_key(XKB_KEY_Escape));
    invoke(group_actions, "photos:create-album");
    REQUIRE(settle_photos(loop, [&] { return library->has_dialog(); }));
    press(photo_key(XKB_KEY_Escape));
    CHECK(library->selected_ids() == selected);
    press(photo_key(XKB_KEY_F6));
    press(photo_key(XKB_KEY_Right));
    draw();
    REQUIRE(browser.sidebar().place_icon_rect_for_uri(weekend_uri));
    REQUIRE(browser.sidebar().place_icon_rect_for_uri(target_uri));
    press(photo_key(XKB_KEY_F6, {.shift = true}));
    // Reopening Albums through its Create action refreshes the list asynchronously.
    REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    REQUIRE(library->destination() == PhotoLibrary::Destination::albums);
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
    CHECK(library->selected_ids().empty());
    CHECK(library->folder()->selected_count() == 0);
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
                    std::pair{"rummage:photos/review", PhotoLibrary::Destination::review}}) {
        click_uri(uri);
        CHECK(library->destination() == destination);
        CHECK(library->folder()->is_collection());
        CHECK(library->folder()->path().empty());
        for (const auto& entry : library->folder()->entries()) {
            CHECK(entry.location.empty());
            CHECK_FALSE(entry.is_directory());
        }
    }
    CHECK_FALSE(browser.sidebar().place_icon_rect_for_uri("rummage:photos/activity"));
    const auto toolbar_menu = library->menu_items();
    CHECK_FALSE(find_menu_item(toolbar_menu, "photos:activity"));
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
    CHECK(std::ranges::none_of(properties.rows, [](const auto& row) {
        return row.second == "0 × 0" || (row.first == "Date origin" && row.second == "Unknown");
    }));
    int offers = 0;
    std::vector<rummage::views::SystemClipboard::Format> copied_formats;
    rummage::views::SystemClipboard clipboard{};
    clipboard.offer = [&](auto formats) {
        ++offers;
        copied_formats = std::move(formats);
    };
    clipboard.owned = [] { return true; };
    clipboard.generation = [&] { return static_cast<std::uint64_t>(offers); };
    browser.set_system_clipboard(std::move(clipboard));
    REQUIRE(browser.key(photo_key(XKB_KEY_Menu), Browser::Clock::now()));
    const auto changed_actions = browser.context_menu().items();
    REQUIRE(browser.key(photo_key(XKB_KEY_Escape), Browser::Clock::now()));
    for (const auto* action_id :
         {"copy", "cut", "copy_path", "properties", "photos:add-to-album"}) {
        const auto action =
            std::ranges::find(changed_actions, action_id, &rummage::views::MenuItem::id);
        REQUIRE(action != changed_actions.end());
        CHECK(action->enabled == (std::string_view{action_id} != "cut"));
    }
    invoke(changed_actions, "properties");
    CHECK(browser.properties_dialog().is_open());
    browser.properties_dialog().close();
    invoke(changed_actions, "copy");
    REQUIRE(settle_photos(loop, [&] {
        return !browser.loading() && std::ranges::any_of(copied_formats, [](const auto& format) {
            return format.first == "image/png";
        });
    }));
    const auto file_offer =
        std::ranges::find(copied_formats, std::string{rummage::views::kGnomeFiles},
                          &rummage::views::SystemClipboard::Format::first);
    REQUIRE(file_offer != copied_formats.end());
    const auto files = rummage::views::parse_gnome_copied_files(file_offer->second);
    REQUIRE(files);
    CHECK_FALSE(files->cut);
    CHECK(files->paths == std::vector<std::filesystem::path>{original});
    const auto image_offer = std::ranges::find(copied_formats, "image/png",
                                               &rummage::views::SystemClipboard::Format::first);
    REQUIRE(image_offer != copied_formats.end());
    std::ifstream copied_original{original, std::ios::binary};
    const std::string current_bytes{std::istreambuf_iterator<char>{copied_original},
                                    std::istreambuf_iterator<char>{}};
    CHECK(image_offer->second == current_bytes);
    const auto copy_offers = offers;
    browser.take_selection(true);
    CHECK(offers == copy_offers);

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
    // Entering a different collection clears the bulk selection and its cached properties.
    // An empty album must never target photographs selected in the library.
    const auto empty_album = catalog.request({{"op", "create_album"}, {"name", "Snapshot check"}});
    REQUIRE(empty_album.value("ok", false));
    library.open_album(empty_album.at("id").get<PhotoLibrary::Id>(), "Snapshot check");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.rows().empty());
    CHECK(library.selected_ids().empty());
    CHECK(library.properties().ids.empty());
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
    invoke(library.menu_items(), "photos:retained");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows().empty());
    CHECK(nlohmann::json::parse(library.state()).at("filter").at("album_id") == album.at("id"));
    CHECK_FALSE(nlohmann::json::parse(library.state()).at("filter").contains("retained_only"));
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
    const auto* retained = find_menu_item(menu, "photos:retained");
    REQUIRE(retained);
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
    invoke(library.menu_items(), "photos:rename-album");
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

TEST_CASE(
    "photo workspace: library and album file copies preserve originals across source folders") {
    rummage::ui::Offscreen offscreen{900, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto first = directory.path / "First source" / "First photo.png";
    const auto second = directory.path / "Second source" / "Second photo.png";
    const auto destination = directory.path / "Copies";
    fixture(first);
    fixture(second);
    std::filesystem::create_directories(destination);
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream input{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto first_bytes = bytes(first), second_bytes = bytes(second);
    rummage::photos::Client catalog;
    for (const auto& source : {first.parent_path(), second.parent_path()})
        REQUIRE(
            catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    nlohmann::json rows;
    for (int i = 0; i < 16; ++i) {
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
        rows = catalog.request({{"op", "query"}}).at("photos");
        if (rows.size() == 2)
            break;
    }
    REQUIRE(rows.size() == 2);
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    const auto album = catalog.request({{"op", "create_album"},
                                        {"name", "Copy test"},
                                        {"ids", {rows[0].at("id"), rows[1].at("id")}}});
    REQUIRE(album.value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    std::vector<rummage::views::SystemClipboard::Format> formats;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto offered) { formats = std::move(offered); };
    clipboard.owned = [] { return true; };
    clipboard.types = [&] {
        std::vector<std::string> types;
        for (const auto& [type, value] : formats)
            types.push_back(type);
        return types;
    };
    clipboard.read = [&](const std::string& type, auto done) {
        const auto found =
            std::ranges::find(formats, type, &rummage::views::SystemClipboard::Format::first);
        REQUIRE(found != formats.end());
        loop.post([done, value = found->second] { done(value); });
    };
    browser.set_system_clipboard(std::move(clipboard));
    browser.open(destination);
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 2; }));
    bool open_album = false, use_menu = false;
    SUBCASE("Library context Copy") { use_menu = true; }
    SUBCASE("Library Ctrl+C") {}
    SUBCASE("Album context Copy") { open_album = use_menu = true; }
    SUBCASE("Album Ctrl+C") { open_album = true; }
    if (open_album) {
        library->open_album(album.at("id"), "Copy test");
        REQUIRE(settle_photos(loop, [&] { return !library->loading(); }));
    }
    browser.set_mode(Browser::Mode::details);
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    renderer.begin_frame(900, 700, 1, {});
    browser.draw(renderer, {0, 0, 900, 700}, {}, icons, Browser::Clock::now());
    renderer.end_frame();
    library->folder()->select_all();
    REQUIRE(library->selected_ids().size() == 2);
    const auto press = [&](KeyEvent key) { REQUIRE(browser.key(key, Browser::Clock::now())); };
    if (use_menu) {
        press(photo_key(XKB_KEY_Menu));
        const auto actions = browser.context_menu().items();
        press(photo_key(XKB_KEY_Escape));
        for (const auto* action_id :
             {"copy", "cut", "copy_path", "properties", "photos:add-to-album"}) {
            const auto action =
                std::ranges::find(actions, action_id, &rummage::views::MenuItem::id);
            REQUIRE(action != actions.end());
            CHECK(action->enabled);
        }
        REQUIRE(!actions.empty());
        CHECK(actions.back().id == "properties");
        CHECK_FALSE(find_menu_item(actions, "photos:edit"));
        CHECK_FALSE(find_menu_item(actions, "photos:create-album"));
        if (!open_album)
            CHECK_FALSE(find_menu_item(actions, "paste"));
        invoke(actions, "copy");
    } else {
        press(photo_key(XKB_KEY_c, {.ctrl = true}));
    }
    REQUIRE(settle_photos(loop, [&] { return !browser.loading() && !formats.empty(); }));
    const auto offered = std::ranges::find(formats, std::string{rummage::views::kGnomeFiles},
                                           &rummage::views::SystemClipboard::Format::first);
    REQUIRE(offered != formats.end());
    const auto copied = rummage::views::parse_gnome_copied_files(offered->second);
    REQUIRE(copied);
    CHECK_FALSE(copied->cut);
    REQUIRE(copied->paths.size() == 2);
    CHECK(std::ranges::find(copied->paths, first) != copied->paths.end());
    CHECK(std::ranges::find(copied->paths, second) != copied->paths.end());
    const auto pasted_album = catalog.request({{"op", "create_album"}, {"name", "Pasted photos"}});
    REQUIRE(pasted_album.value("ok", false));
    library->open_album(pasted_album.at("id").get<PhotoLibrary::Id>(), "Pasted photos");
    REQUIRE(settle_photos(loop, [&] { return !library->loading() && library->rows().empty(); }));
    if (use_menu) {
        press(photo_key(XKB_KEY_Menu));
        const auto paste_actions = browser.context_menu().items();
        press(photo_key(XKB_KEY_Escape));
        invoke(paste_actions, "paste");
    } else {
        press(photo_key(XKB_KEY_v, {.ctrl = true}));
    }
    REQUIRE(settle_photos(loop, [&] {
        return !library->loading() && library->rows().size() == 2 &&
               catalog.request({{"op", "query"}, {"album_id", pasted_album.at("id")}})
                       .value("total", 0) == 2;
    }));
    // Pasting in an album changes membership; the same clipboard still copies files to a folder.
    CHECK(bytes(first) == first_bytes);
    CHECK(bytes(second) == second_bytes);
    browser.open(destination);
    REQUIRE(settle_photos(loop, [&] { return !browser.loading(); }));
    CHECK_FALSE(browser.showing_photos());
    press(photo_key(XKB_KEY_v, {.ctrl = true}));
    REQUIRE(settle_photos(loop, [&] {
        return !browser.loading() && std::filesystem::exists(destination / first.filename()) &&
               std::filesystem::exists(destination / second.filename());
    }));
    CHECK(bytes(destination / first.filename()) == first_bytes);
    CHECK(bytes(destination / second.filename()) == second_bytes);
    CHECK(bytes(first) == first_bytes);
    CHECK(bytes(second) == second_bytes);
    CHECK(catalog.request({{"op", "query"}, {"album_id", album.at("id")}}).value("total", 0) == 2);
}

TEST_CASE(
    "photo workspace: delayed image clipboard enrichment never replaces newer clipboard data") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto original = directory.path / "Photos" / "Image.png";
    fixture(original);
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", original.parent_path().string()}})
                .value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    // One worker makes the marker below a deterministic barrier after the image read.
    rummage::core::WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 1; }));
    library->folder()->set_cursor(0);
    std::vector<rummage::views::SystemClipboard::Format> formats;
    std::uint64_t generation = 0;
    int offers = 0;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto value) {
        formats = std::move(value);
        ++generation;
        ++offers;
    };
    clipboard.owned = [] { return true; };
    clipboard.generation = [&] { return generation; };
    browser.set_system_clipboard(std::move(clipboard));
    const auto drain_image_work = [&] {
        bool complete = false;
        pool.submit(
            rummage::core::Priority::prefetch, [](const auto&) { return true; },
            [&](bool ready) { complete = ready; });
        REQUIRE(settle_photos(loop, [&] { return complete; }));
    };
    browser.take_selection(false);
    browser.copy_text("New text");
    const auto text_formats = formats;
    const auto text_offers = offers;
    drain_image_work();
    CHECK(formats == text_formats);
    CHECK(offers == text_offers);

    browser.take_selection(false);
    // Simulate another application's ownership generation without calling clipboard_lost().
    ++generation;
    formats = {{"text/plain", "External clipboard"}};
    const auto external_formats = formats;
    const auto external_offers = offers;
    drain_image_work();
    CHECK(formats == external_formats);
    CHECK(offers == external_offers);
}

TEST_CASE("photo workspace: JPEG copy offers original bytes and a full upright PNG") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    const auto original = source / "Rotated photo.jpg";
    std::filesystem::create_directories(source);
    std::filesystem::copy_file(
        std::filesystem::path{RUMMAGE_TEST_DATA} / "image/blocks-orient6.jpg", original);
    std::ifstream input{original, std::ios::binary};
    const std::string original_bytes{std::istreambuf_iterator<char>{input},
                                     std::istreambuf_iterator<char>{}};
    REQUIRE(!original_bytes.empty());
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 1; }));
    library->folder()->set_cursor(0);
    std::vector<rummage::views::SystemClipboard::Format> formats;
    std::uint64_t generation = 0;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto value) {
        formats = std::move(value);
        ++generation;
    };
    clipboard.owned = [] { return true; };
    clipboard.generation = [&] { return generation; };
    browser.set_system_clipboard(std::move(clipboard));
    browser.take_selection(false);
    REQUIRE(settle_photos(loop, [&] {
        return std::ranges::any_of(formats,
                                   [](const auto& format) { return format.first == "image/png"; });
    }));
    const auto offered = [&](std::string_view type) -> const std::string& {
        const auto found =
            std::ranges::find(formats, type, &rummage::views::SystemClipboard::Format::first);
        REQUIRE(found != formats.end());
        return found->second;
    };
    CHECK(offered("image/jpeg") == original_bytes);
    const auto files =
        rummage::views::parse_gnome_copied_files(offered(rummage::views::kGnomeFiles));
    REQUIRE(files);
    CHECK_FALSE(files->cut);
    CHECK(files->paths == std::vector<std::filesystem::path>{original});
    const auto& png_bytes = offered("image/png");
    std::string error;
    const auto png = rummage::picture::decode_png(
        {reinterpret_cast<const std::uint8_t*>(png_bytes.data()), png_bytes.size()}, error);
    REQUIRE(png);
    CHECK(error.empty());
    CHECK(png->width == 32);
    CHECK(png->height == 48);
    REQUIRE(png->rgba.size() == 32 * 48 * 4);
    // Orientation 6 is a clockwise quarter-turn: white top-left, red top-right, blue bottom-right.
    const auto channel = [&](std::size_t x, std::size_t y, std::size_t c) {
        return png->rgba[(y * png->width + x) * 4 + c];
    };
    CHECK(channel(8, 8, 0) > 240);
    CHECK(channel(8, 8, 2) > 240);
    CHECK(channel(24, 8, 0) > 240);
    CHECK(channel(24, 8, 1) < 16);
    CHECK(channel(24, 40, 2) > 240);
    CHECK(channel(24, 40, 0) < 16);
}

TEST_CASE("photo workspace: album paste reads shared clipboard and respects KDE cut markers") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    const auto first = source / "First.png", second = source / "Second.png";
    fixture(first);
    fixture(second);
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream input{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto first_bytes = bytes(first), second_bytes = bytes(second);
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    const auto shared_album =
        catalog.request({{"op", "create_album"}, {"name", "Shared clipboard"}});
    const auto kde_album = catalog.request({{"op", "create_album"}, {"name", "KDE clipboard"}});
    REQUIRE(shared_album.value("ok", false));
    REQUIRE(kde_album.value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](auto) {}};
    browser.open_photo_library();
    auto* library = browser.photo_library();
    REQUIRE(library);
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 2; }));
    std::vector<rummage::views::SystemClipboard::Format> formats;
    std::uint64_t generation = 0;
    int delivered = 0;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto value) {
        formats = std::move(value);
        ++generation;
    };
    clipboard.owned = [] { return true; };
    clipboard.generation = [&] { return generation; };
    clipboard.types = [&] {
        std::vector<std::string> types;
        for (const auto& [type, value] : formats)
            types.push_back(type);
        return types;
    };
    clipboard.read = [&](const std::string& type, auto done) {
        const auto found =
            std::ranges::find(formats, type, &rummage::views::SystemClipboard::Format::first);
        REQUIRE(found != formats.end());
        loop.post([&, done, value = found->second] {
            done(value);
            ++delivered;
        });
    };
    browser.set_system_clipboard(std::move(clipboard));
    library->folder()->set_cursor(0);
    REQUIRE(library->focused_photo().at("path") == first.string());
    browser.take_selection(false);
    REQUIRE(!formats.empty());
    // A second Rummage window changes the shared clipboard; owned() still returns true.
    ++generation;
    formats = {{std::string{rummage::views::kUriList},
                rummage::views::uri_list(std::vector<std::filesystem::path>{second})}};
    library->open_album(shared_album.at("id").get<PhotoLibrary::Id>(), "Shared clipboard");
    REQUIRE(settle_photos(loop, [&] { return !library->loading() && library->rows().empty(); }));
    browser.paste();
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 1; }));
    CHECK(library->rows().front().at("path") == second.string());

    library->open_album(kde_album.at("id").get<PhotoLibrary::Id>(), "KDE clipboard");
    REQUIRE(settle_photos(loop, [&] { return !library->loading() && library->rows().empty(); }));
    ++generation;
    formats = {{std::string{rummage::views::kUriList},
                rummage::views::uri_list(std::vector<std::filesystem::path>{first})},
               {std::string{rummage::views::kKdeCut}, "1"}};
    delivered = 0;
    browser.paste();
    REQUIRE(settle_photos(loop, [&] { return delivered == 2 && !library->loading(); }));
    CHECK(library->rows().empty());
    CHECK(catalog.request({{"op", "query"}, {"album_id", kde_album.at("id")}}).value("total", -1) ==
          0);
    CHECK(bytes(first) == first_bytes);
    CHECK(bytes(second) == second_bytes);
    ++generation;
    formats.back().second = "0";
    browser.paste();
    REQUIRE(
        settle_photos(loop, [&] { return !library->loading() && library->rows().size() == 1; }));
    CHECK(library->rows().front().at("path") == first.string());
    CHECK(bytes(first) == first_bytes);
    CHECK(bytes(second) == second_bytes);
}

TEST_CASE("photo workspace: restored catalog state is bound to catalog identity, not reused IDs") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto old_source = directory.path / "Old source",
               new_source = directory.path / "New source";
    fixture(old_source / "Old.png");
    fixture(new_source / "New.png");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    nlohmann::json saved;
    PhotoLibrary::Id old_id = 0;
    const auto seed = [&](const std::filesystem::path& source) {
        rummage::photos::Client catalog;
        REQUIRE(
            catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
        REQUIRE(
            catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
        const auto rows = catalog.request({{"op", "query"}}).at("photos");
        REQUIRE(rows.size() == 1);
        const auto album = catalog.request(
            {{"op", "create_album"}, {"name", "Album"}, {"ids", {rows[0].at("id")}}});
        REQUIRE(album.value("ok", false));
        REQUIRE(catalog
                    .request({{"op", "save_filter"},
                              {"name", "Saved album"},
                              {"filter", {{"album_id", album.at("id")}}}})
                    .value("ok", false));
        return rows[0].at("id").get<PhotoLibrary::Id>();
    };
    {
        const rummage::test::ScopedEnv data{"XDG_DATA_HOME",
                                            (directory.path / "old-data").string()};
        old_id = seed(old_source);
        PhotoLibrary library{loop, pool, watcher, mime, [] {}};
        library.open();
        REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
        const auto search = library.client()->request({{"op", "filters"}}).at("filters")[0];
        library.open_saved_search(search);
        REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
        library.folder()->set_cursor(0);
        saved = nlohmann::json::parse(library.state());
        CHECK(saved.at("version") == 3);
        CHECK_FALSE(saved.at("catalog_identity").get<std::string>().empty());
        CHECK(saved.at("selected") == nlohmann::json::array({old_id}));
        PhotoLibrary matching{loop, pool, watcher, mime, [] {}};
        matching.restore_state(saved.dump());
        CHECK(matching.selected_ids().empty());
        matching.set_sort({});  // Browser tab restoration can query before open().
        matching.open();
        REQUIRE(settle_photos(loop, [&] { return !matching.loading(); }));
        CHECK(matching.selected_ids() == nlohmann::json::array({old_id}));
        REQUIRE(matching.folder()->cursor_entry());
        CHECK(matching.folder()->cursor_entry()->key() == "photo:" + std::to_string(old_id));
        CHECK(matching.title() == "Search: Saved album");
        CHECK(nlohmann::json::parse(matching.state()).at("catalog_identity") ==
              saved.at("catalog_identity"));
        auto legacy = saved;
        legacy["version"] = 2;
        legacy.erase("catalog_identity");
        PhotoLibrary unbound{loop, pool, watcher, mime, [] {}};
        unbound.restore_state(legacy.dump());
        unbound.open();
        REQUIRE(settle_photos(loop, [&] { return !unbound.loading(); }));
        CHECK(unbound.selected_ids().empty());
        CHECK(unbound.at_all_photos());
    }
    {
        const rummage::test::ScopedEnv data{"XDG_DATA_HOME",
                                            (directory.path / "new-data").string()};
        const auto new_id = seed(new_source);
        REQUIRE(new_id ==
                old_id);  // IDs collide across catalogs: existence checks are insufficient.
        saved["selected"] = {old_id, 80077, 109568};
        PhotoLibrary replacement{loop, pool, watcher, mime, [] {}};
        replacement.restore_state(saved.dump());
        replacement.set_sort({});
        CHECK(replacement.selected_ids().empty());
        replacement.open();
        REQUIRE(settle_photos(loop, [&] { return !replacement.loading(); }));
        REQUIRE(replacement.rows().size() == 1);
        CHECK(replacement.rows()[0].at("path") == (new_source / "New.png").string());
        CHECK(replacement.at_all_photos());
        CHECK(replacement.selected_ids().empty());
        CHECK(replacement.folder()->selected_count() == 0);
        CHECK_FALSE(replacement.folder()->cursor());
        const auto clean = nlohmann::json::parse(replacement.state());
        CHECK(clean.at("catalog_identity") != saved.at("catalog_identity"));
        CHECK(clean.at("saved_search_id") == 0);
        CHECK(clean.at("cursor") == 0);
        CHECK_FALSE(clean.at("filter").contains("album_id"));
        invoke(replacement.menu_items(), "photos:create-album");
        REQUIRE(settle_photos(loop, [&] { return replacement.has_dialog(); }));
        replacement.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, "Empty after reset"));
        replacement.key_dialog(photo_key(XKB_KEY_Return));
        REQUIRE(settle_photos(loop, [&] { return !replacement.loading(); }));
        const auto albums = replacement.client()->request({{"op", "albums"}}).at("albums");
        const auto empty = std::ranges::find_if(albums, [](const auto& album) {
            return album.value("name", "") == "Empty after reset";
        });
        REQUIRE(empty != albums.end());
        CHECK(empty->at("count") == 0);
        replacement.folder()->set_cursor(0);
        invoke(replacement.menu_items(), "photos:create-album");
        REQUIRE(settle_photos(loop, [&] { return replacement.has_dialog(); }));
        replacement.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, "Current photo"));
        replacement.key_dialog(photo_key(XKB_KEY_Return));
        REQUIRE(settle_photos(loop, [&] { return !replacement.loading(); }));
        const auto with_photo = replacement.client()->request({{"op", "albums"}}).at("albums");
        const auto current = std::ranges::find_if(with_photo, [](const auto& album) {
            return album.value("name", "") == "Current photo";
        });
        REQUIRE(current != with_photo.end());
        CHECK(current->at("count") == 1);
    }
}

TEST_CASE(
    "photo workspace: album creation prunes forgotten source selections and requires review") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto first = directory.path / "Forgotten" / "Old.png";
    const auto second = directory.path / "Retained" / "Current.png";
    fixture(first);
    fixture(second);
    rummage::photos::Client catalog;
    const auto old_source =
        catalog.request({{"op", "add_source"}, {"path", first.parent_path().string()}});
    REQUIRE(old_source.value("ok", false));
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", second.parent_path().string()}})
                .value("ok", false));
    for (int i = 0; i < 16 && catalog.request({{"op", "query"}}).value("total", 0) < 2; ++i)
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 2; }));
    library.folder()->select_all();
    REQUIRE(library.selected_ids().size() == 2);
    // A source forgotten externally can leave a pending bulk selection in this same view.
    // Preflight must still catch missing IDs without silently creating a smaller album.
    const auto sources = catalog.request({{"op", "sources"}}).at("sources");
    const auto old = std::ranges::find_if(sources, [&](const auto& source) {
        return source.value("path", "") == first.parent_path().string();
    });
    REQUIRE(old != sources.end());
    const auto source_id = old->at("id");
    REQUIRE(catalog.request({{"op", "remove_source"}, {"id", source_id}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "forget_source"}, {"id", source_id}}).value("ok", false));
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 1; }));
    REQUIRE(library.selected_ids().size() == 2);
    invoke(library.menu_items(), "photos:create-album");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.has_dialog());
    CHECK(library.status().find("no longer exists in the catalog") != std::string::npos);
    REQUIRE(library.selected_ids().size() == 1);
    CHECK(library.selected_ids()[0] == library.rows()[0].at("id"));
    CHECK(library.selected_photos().size() == 1);
    CHECK(catalog.request({{"op", "albums"}}).at("albums").empty());
    library.key_dialog(photo_key(XKB_KEY_Return));
    CHECK_FALSE(library.has_dialog());
    CHECK(catalog.request({{"op", "albums"}}).at("albums").empty());
    invoke(library.menu_items(), "photos:create-album");
    REQUIRE(settle_photos(loop, [&] { return library.has_dialog(); }));
    library.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, "Reviewed selection"));
    library.key_dialog(photo_key(XKB_KEY_Return));
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    const auto albums = catalog.request({{"op", "albums"}}).at("albums");
    REQUIRE(albums.size() == 1);
    CHECK(albums[0].at("name") == "Reviewed selection");
    CHECK(albums[0].at("count") == 1);
    CHECK(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
}

TEST_CASE("photo workspace: album write failures keep the entered name in a visible dialog") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "Image.png");
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 1; }));
    const auto enter_name = [&](std::string name) {
        library.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, std::move(name)));
        library.key_dialog(photo_key(XKB_KEY_Return));
    };
    invoke(library.menu_items(), "photos:create-album");
    REQUIRE(library.has_dialog());
    enter_name("Food & drink");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(library.has_dialog());
    auto albums = catalog.request({{"op", "albums"}}).at("albums");
    REQUIRE(albums.size() == 1);
    CHECK(albums[0].at("name") == "Food & drink");

    invoke(library.menu_items(), "photos:create-album");
    enter_name("Food & drink");
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.has_dialog(); }));
    CHECK(library.copy_selection(false) == "Food & drink");
    CHECK(library.status().find("An album with this name already exists") != std::string::npos);
    CHECK(catalog.request({{"op", "albums"}}).at("albums").size() == 1);
    library.key_dialog(photo_key(XKB_KEY_Escape));

    library.folder()->set_cursor(0);
    invoke(library.menu_items(), "photos:create-album");
    REQUIRE(settle_photos(loop, [&] { return library.has_dialog(); }));
    // The selection existed during preflight but disappears before the user confirms its name.
    const auto source_id = catalog.request({{"op", "sources"}}).at("sources")[0].at("id");
    REQUIRE(catalog.request({{"op", "remove_source"}, {"id", source_id}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "forget_source"}, {"id", source_id}}).value("ok", false));
    enter_name("Keep this name");
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.has_dialog(); }));
    CHECK(library.copy_selection(false) == "Keep this name");
    CHECK(library.status().find("no longer exists in the catalog") != std::string::npos);
    CHECK(catalog.request({{"op", "albums"}}).at("albums").size() == 1);
    CHECK(std::filesystem::exists(source / "Image.png"));
    library.key_dialog(photo_key(XKB_KEY_Escape));
    invoke(library.menu_items(), "photos:clear-selection");

    invoke(library.menu_items(), "photos:create-album");
    enter_name("Food & drink");
    library.show(PhotoLibrary::Destination::albums);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(library.has_dialog());  // A late failure never reopens after navigation.

    invoke(library.menu_items(), "photos:create-album");
    enter_name("Food & drink");
    invoke(library.menu_items(), "photos:create-album");
    library.key_dialog(photo_key(XKB_KEY_NoSymbol, {}, "New dialog"));
    library.key_dialog(photo_key(XKB_KEY_a, {.ctrl = true}));
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.has_dialog());
    CHECK(library.copy_selection(false) == "New dialog");
    library.key_dialog(photo_key(XKB_KEY_Escape));
    CHECK(catalog.request({{"op", "albums"}}).at("albums").size() == 1);
}

TEST_CASE(
    "photo workspace: navigation-only catalog initialization does not start background work") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "Unindexed.png");
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    const auto before = catalog.request({{"op", "jobs"}}).at("jobs");
    REQUIRE_FALSE(before.empty());
    REQUIRE(catalog.request({{"op", "query"}}).value("total", -1) == 0);
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.refresh_navigation();
    REQUIRE(settle_photos(loop, [&] {
        return !library.loading() && library.navigation().at("sources").size() == 1;
    }));
    CHECK_FALSE(library.is_open());
    // Cover two normal worker timer intervals; navigation alone must not advance queued jobs.
    const auto observation = loop.add_timer(600ms, 0ms, [&] { loop.quit(0); });
    loop.run();
    loop.remove(observation);
    CHECK(catalog.request({{"op", "query"}}).value("total", -1) == 0);
    CHECK(catalog.request({{"op", "jobs"}}).at("jobs") == before);
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 1; }));
}

TEST_CASE(
    "photo workspace: compact context menus keep collection tools in native toolbar submenus") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "First.png");
    fixture(source / "Second.png");
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    const auto rows = catalog.request({{"op", "query"}}).at("photos");
    REQUIRE(rows.size() == 2);
    const auto album = catalog.request(
        {{"op", "create_album"}, {"name", "Both"}, {"ids", {rows[0].at("id"), rows[1].at("id")}}});
    REQUIRE(album.value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.set_folder_chooser([](auto, auto) {});
    library.set_album_chooser([](auto, auto) {});
    library.set_edit_handler([] {});
    library.set_reveal_handler([](auto) {});
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    const auto toolbar = library.menu_items();
    CHECK(toolbar.size() == 7);
    for (const auto& item : toolbar)
        CHECK_FALSE(item.id.starts_with("photos:destination:"));
    for (const auto* group : {"photos:search-menu", "photos:selection-menu",
                              "photos:background-menu", "photos:annotations-menu"}) {
        const auto* submenu = find_menu_item(toolbar, group);
        REQUIRE(submenu);
        CHECK(submenu->submenu);
        CHECK_FALSE(submenu->children.empty());
    }
    const auto empty_context = library.context_menu_items();
    REQUIRE(empty_context.size() == 1);
    CHECK(empty_context[0].id == "photos:select-page");
    // Navigate the actual native submenu with keys and run its Select loaded photos action.
    rummage::views::ContextMenu native_menu{[] {}};
    native_menu.open(10, 10, toolbar, true);
    const auto selection =
        std::ranges::find(toolbar, "photos:selection-menu", &rummage::views::MenuItem::id);
    REQUIRE(selection != toolbar.end());
    for (auto item = toolbar.begin(); item != selection; ++item)
        REQUIRE(native_menu.key(photo_key(XKB_KEY_Down)));
    REQUIRE(native_menu.key(photo_key(XKB_KEY_Right)));
    REQUIRE(native_menu.submenu_open());
    CHECK(native_menu.sub_highlighted() == 2);
    REQUIRE(native_menu.key(photo_key(XKB_KEY_Return)));
    CHECK_FALSE(native_menu.is_open());
    REQUIRE(library.selected_ids().size() == 2);
    const auto selected_context = library.context_menu_items();
    CHECK(selected_context.size() == 2);
    CHECK(find_menu_item(selected_context, "photos:reveal"));
    CHECK(find_menu_item(selected_context, "photos:add-to-album"));
    for (const auto* id : {"photos:edit", "photos:create-album", "photos:filters",
                           "photos:retained", "photos:select-page", "photos:refresh",
                           "photos:pause", "photos:export", "photos:restore-originals"})
        CHECK_FALSE(find_menu_item(selected_context, id));
    library.open_album(album.at("id").get<PhotoLibrary::Id>(), "Both");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(find_menu_item(library.context_menu_items(), "photos:set-album-cover"));
    library.folder()->set_cursor(0);
    const auto single = library.context_menu_items();
    CHECK(find_menu_item(single, "photos:set-album-cover"));
    CHECK(find_menu_item(single, "photos:album-remove"));
    CHECK_FALSE(find_menu_item(single, "photos:rename-album"));
    CHECK_FALSE(find_menu_item(single, "photos:delete-album"));
    library.folder()->select_all();
    const auto captured = library.context_menu_items();
    library.folder()->set_cursor(0);
    invoke(captured, "photos:album-remove");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.rows()
              .empty());  // The captured two-photo selection was removed, not the later one.
    CHECK(catalog.request({{"op", "query"}}).value("total", 0) == 2);
    CHECK(std::filesystem::exists(source / "First.png"));
    CHECK(std::filesystem::exists(source / "Second.png"));
    library.show(PhotoLibrary::Destination::albums);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.folder()->set_cursor(0);
    const auto record_context = library.context_menu_items();
    REQUIRE(!record_context.empty());
    CHECK(record_context.front().id == "photos:show-album");
    std::set<std::string> record_ids;
    for (const auto& item : record_context)
        CHECK(record_ids.insert(item.id).second);
    CHECK(find_menu_item(record_context, "photos:rename-album"));
    CHECK(find_menu_item(record_context, "photos:delete-album"));
    CHECK(find_menu_item(record_context, "photos:reset-album-cover"));
    library.folder()->set_cursor(std::nullopt);
    const auto albums_background = library.context_menu_items();
    REQUIRE(albums_background.size() == 1);
    CHECK(albums_background[0].id == "photos:create-album");
    library.show(PhotoLibrary::Destination::activity);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(!library.folder()->entries().empty());
    library.folder()->set_cursor(0);
    const auto jobs_before = catalog.request({{"op", "jobs"}}).at("jobs");
    CHECK(library.activate());
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(catalog.request({{"op", "jobs"}}).at("jobs") == jobs_before);
    library.show(PhotoLibrary::Destination::sources);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.folder()->set_cursor(0);
    const auto active_source = library.context_menu_items();
    CHECK(active_source.front().id == "photos:show-source");
    CHECK(find_menu_item(active_source, "photos:inactive-source"));
    CHECK_FALSE(find_menu_item(active_source, "photos:forget-source"));
    invoke(active_source, "photos:inactive-source");
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.folder()->set_cursor(0);
    const auto inactive_source = library.context_menu_items();
    CHECK(find_menu_item(inactive_source, "photos:forget-source"));
    CHECK_FALSE(find_menu_item(inactive_source, "photos:inactive-source"));
    CHECK_FALSE(find_menu_item(inactive_source, "photos:refresh-source"));
    library.folder()->set_cursor(std::nullopt);
    const auto sources_background = library.context_menu_items();
    REQUIRE(sources_background.size() == 1);
    CHECK(sources_background[0].id == "photos:add");
}

TEST_CASE("photo workspace: photo selections are scoped to the visible collection") {
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    for (int i = 0; i < 8; ++i)
        fixture(source / ("photo-" + std::to_string(i) + ".png"));
    rummage::photos::Client catalog;
    const auto added = catalog.request({{"op", "add_source"}, {"path", source.string()}});
    REQUIRE(added.value("ok", false));
    for (int i = 0; i < 16 && catalog.request({{"op", "query"}}).value("total", 0) < 8; ++i)
        REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    const auto photos = catalog.request({{"op", "query"}}).at("photos");
    REQUIRE(photos.size() == 8);
    nlohmann::json members = nlohmann::json::array();
    for (std::size_t i = 0; i < 5; ++i)
        members.push_back(photos[i].at("id"));
    const auto album =
        catalog.request({{"op", "create_album"}, {"name", "Countryside"}, {"ids", members}});
    REQUIRE(album.value("ok", false));
    const auto saved = catalog.request({{"op", "save_filter"},
                                        {"name", "Countryside search"},
                                        {"filter", {{"album_id", album.at("id")}}}});
    REQUIRE(saved.value("ok", false));
    const auto search = catalog.request({{"op", "filters"}}).at("filters").front();
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 8; }));
    const auto folder = library.folder();
    const std::array<std::size_t, 3> previous{0, 6, 7};
    folder->select_rows(previous, false);
    REQUIRE(library.selected_ids().size() == 3);
    library.show_all_photos();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK(library.selected_ids().size() == 3);  // Reopening the same scope preserves selection.

    library.open_album(album.at("id"), "Countryside");
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 5; }));
    CHECK(library.selected_ids().empty());
    CHECK(folder->selected_count() == 0);
    CHECK(library.properties().ids.empty());
    folder->set_cursor(1);
    const auto expect_selection = [&](std::size_t count) {
        CHECK(folder->selected_count() == count);
        CHECK(library.selected_ids().size() == count);
        CHECK(library.selected_photos().size() == count);
        CHECK(library.properties().ids.size() == count);
        CHECK(library.status().find(std::to_string(count) + " selected") != std::string::npos);
    };
    expect_selection(1);
    CHECK(library.selected_ids()[0] == library.rows()[1].at("id"));
    folder->set_cursor(3, rummage::views::SelectMode::toggle);
    expect_selection(2);  // Ctrl-click preserves the current collection's other selected row.
    folder->move_cursor(1, rummage::views::SelectMode::replace);
    expect_selection(1);  // Plain arrow movement replaces every bulk target.
    CHECK(library.selected_ids()[0] == library.rows()[4].at("id"));
    library.refresh();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    expect_selection(1);
    library.show_all_photos();
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 8; }));
    expect_selection(0);
    folder->set_cursor(0);
    library.open_source(added.at("source").at("id"), source.string());
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    expect_selection(0);
    folder->set_cursor(0);
    library.open_saved_search(search);
    REQUIRE(settle_photos(loop, [&] { return !library.loading() && library.rows().size() == 5; }));
    expect_selection(0);

    // Old saved sessions may already contain out-of-view IDs. The first completed restored query
    // must discard them before any new gesture, so the inspector never targets hidden photos.
    folder->set_cursor(0);
    auto state = nlohmann::json::parse(library.state());
    state["selected"] = {photos[0].at("id"), photos[6].at("id"), photos[7].at("id")};
    PhotoLibrary restored{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    restored.restore_state(state.dump());
    restored.open();
    REQUIRE(settle_photos(loop, [&] { return !restored.loading(); }));
    REQUIRE(restored.selected_ids().size() == 1);
    REQUIRE(restored.folder()->selected_count() == 1);
    restored.folder()->set_cursor(0);
    CHECK(restored.selected_ids().size() == 1);
    CHECK(restored.properties().ids.size() == 1);
    CHECK(restored.selected_photos().size() == 1);
}

TEST_CASE("photo workspace: source relink review cancels safely and requires an explicit submit") {
    rummage::ui::Offscreen offscreen{800, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Original";
    const auto moved = directory.path / "Moved";
    fixture(source / "Photo.png");
    rummage::photos::Client catalog;
    REQUIRE(catalog.request({{"op", "add_source"}, {"path", source.string()}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "work"}, {"budget", 32}}).value("ok", false));
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    const auto original = catalog.request({{"op", "query"}}).at("photos").front();
    const auto album = catalog.request(
        {{"op", "create_album"}, {"name", "Keep associations"}, {"ids", {original.at("id")}}});
    REQUIRE(album.value("ok", false));
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.show(PhotoLibrary::Destination::sources);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.folder()->entries().size() == 1);
    library.folder()->set_cursor(0);
    std::function<void(std::filesystem::path)> choose;
    library.set_folder_chooser([&](const auto& start, auto completion) {
        CHECK(std::filesystem::path{start} == source);
        choose = std::move(completion);
    });
    std::filesystem::rename(source, moved);
    invoke(library.context_menu_items(), "photos:relink-source");
    REQUIRE(static_cast<bool>(choose));
    choose(moved);
    REQUIRE(library.has_dialog());
    // Cancel before the worker callback returns: its stale preview must not reopen the form.
    library.key_dialog(photo_key(XKB_KEY_Escape));
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(library.has_dialog());
    CHECK(catalog.request({{"op", "sources"}}).at("sources")[0].at("path") == source.string());

    invoke(library.context_menu_items(), "photos:relink-source");
    choose(moved);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.has_dialog());
    CHECK(catalog.request({{"op", "sources"}}).at("sources")[0].at("path") == source.string());
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    renderer.begin_frame(800, 700, 1, {});
    library.draw_dialogs(renderer, {0, 0, 800, 700}, {});
    renderer.end_frame();
    capture("native-source-relink-review", 800, 700);
    CHECK(library.control_bounds("Reconnect").empty());
    const auto button = library.control_bounds("Relink");
    REQUIRE_FALSE(button.empty());
    library.pointer_dialog({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = button.x + button.width / 2,
                            .y = button.y + button.height / 2,
                            .button = rummage::ui::PointerButton::left});
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(library.has_dialog());
    CHECK(catalog.request({{"op", "sources"}}).at("sources")[0].at("path") == moved.string());
    const auto result =
        catalog.request({{"op", "query"}, {"album_id", album.at("id")}}).at("photos");
    REQUIRE(result.size() == 1);
    CHECK(result[0].at("id") == original.at("id"));
    CHECK(result[0].at("path") == (moved / "Photo.png").string());
    CHECK(std::filesystem::exists(moved / "Photo.png"));
}

TEST_CASE("photo workspace: legacy device remount requires native Reconnect confirmation") {
    rummage::ui::Offscreen offscreen{800, 700};
    const rummage::test::TempDir directory;
    const rummage::test::ScopedEnv data{"XDG_DATA_HOME", (directory.path / "data").string()};
    const rummage::test::ScopedEnv cache{"XDG_CACHE_HOME", (directory.path / "cache").string()};
    const rummage::test::ScopedEnv module{"RUMMAGE_PHOTOS_LIBRARY", RUMMAGE_PHOTOS_TEST_MODULE};
    const auto source = directory.path / "Pictures";
    fixture(source / "First.png");
    fixture(source / "Second.png");
    rummage::photos::Client catalog;
    const auto added = catalog.request({{"op", "add_source"}, {"path", source.string()}});
    REQUIRE(added.value("ok", false));
    for (int i = 0; i < 12; ++i) {
        const auto work = catalog.request({{"op", "work"}, {"budget", 32}});
        REQUIRE(work.value("ok", false));
        if (!work.value("worked", false))
            break;
    }
    REQUIRE(catalog.request({{"op", "background_control"}, {"paused", true}}).value("ok", false));
    auto original = catalog.request({{"op", "query"}}).at("photos");
    REQUIRE(original.size() == 2);
    REQUIRE(original[0].value("width", 0) > 0);
    const auto album = catalog.request({{"op", "create_album"},
                                        {"name", "Countryside"},
                                        {"ids", {original[0].at("id"), original[1].at("id")}}});
    REQUIRE(album.value("ok", false));
    REQUIRE(catalog
                .request({{"op", "edit"},
                          {"ids", {original[0].at("id")}},
                          {"description", "Retained annotation"},
                          {"rating", 4}})
                .value("ok", false));
    original = catalog.request({{"op", "query"}, {"album_id", album.at("id")}}).at("photos");

    sqlite3* connection = nullptr;
    REQUIRE(sqlite3_open(rummage::photos::Client::default_catalog_path().c_str(), &connection) ==
            SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{connection, sqlite3_close};
    const auto scalar = [&](const char* query) {
        sqlite3_stmt* statement = nullptr;
        REQUIRE(sqlite3_prepare_v2(connection, query, -1, &statement, nullptr) == SQLITE_OK);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> cleanup{statement,
                                                                                 sqlite3_finalize};
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        return sqlite3_column_int64(statement, 0);
    };
    if (scalar("SELECT count(*) FROM source_filesystems WHERE identity<>''") == 0) {
        MESSAGE(
            "Skipping native remount confirmation: fixture filesystem has no stable local UUID "
            "(for example overlay or FUSE).");
        return;
    }
    const auto actual_device = scalar("SELECT device FROM sources");
    // Reproduce an older catalog after a device-number-only change. This touches only the
    // private fixture database; source paths, inodes, content, and timestamps stay unchanged.
    const auto sql =
        "BEGIN; DELETE FROM source_filesystems; "
        "UPDATE sources SET device=device+1000; "
        "UPDATE photos SET metadata_signature=CASE WHEN metadata_signature<>'' THEN "
        "CAST(device+1000 AS TEXT)||substr(metadata_signature,instr(metadata_signature,':')) "
        "ELSE '' END,device=device+1000; COMMIT;";
    REQUIRE(sqlite3_exec(connection, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    const auto preview = catalog.request({{"op", "relink_source"},
                                          {"source_id", added.at("source").at("id")},
                                          {"path", source.string()},
                                          {"confirm", false}});
    REQUIRE(preview.value("ok", false));
    REQUIRE(preview.value("requires_confirmation", false));
    REQUIRE(preview.at("remount_candidates").size() == 2);
    REQUIRE(preview.at("missing").empty());
    REQUIRE(preview.at("ambiguous").empty());

    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    PhotoLibrary library{loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}};
    library.open();
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    library.show(PhotoLibrary::Destination::sources);
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    REQUIRE(library.folder()->entries().size() == 1);
    library.folder()->set_cursor(0);
    library.set_folder_chooser([&](const auto& start, auto completion) {
        CHECK(std::filesystem::path{start} == source);
        completion(source);
    });
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    const auto review = [&] {
        invoke(library.context_menu_items(), "photos:relink-source");
        REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
        REQUIRE(library.has_dialog());
        renderer.begin_frame(800, 700, 1, {});
        library.draw_dialogs(renderer, {0, 0, 800, 700}, {});
        renderer.end_frame();
        REQUIRE_FALSE(library.control_bounds("Reconnect").empty());
        CHECK(library.control_bounds("Relink").empty());
        CHECK(scalar("SELECT device FROM sources") == actual_device + 1000);
    };
    review();
    capture("native-source-remount-review", 800, 700);
    library.key_dialog(photo_key(XKB_KEY_Escape));
    CHECK_FALSE(library.has_dialog());
    CHECK(scalar("SELECT device FROM sources") == actual_device + 1000);
    CHECK(scalar("SELECT count(*) FROM source_filesystems") == 0);
    review();
    const auto button = library.control_bounds("Reconnect");
    library.pointer_dialog({.kind = rummage::ui::PointerEvent::Kind::press,
                            .x = button.x + button.width / 2,
                            .y = button.y + button.height / 2,
                            .button = rummage::ui::PointerButton::left});
    REQUIRE(settle_photos(loop, [&] { return !library.loading(); }));
    CHECK_FALSE(library.has_dialog());
    CHECK(scalar("SELECT device FROM sources") == actual_device);
    CHECK(scalar("SELECT count(*) FROM source_filesystems WHERE identity<>''") == 1);
    const auto sources = catalog.request({{"op", "sources"}}).at("sources");
    CHECK(sources[0].at("availability") == "online");
    const auto restored =
        catalog.request({{"op", "query"}, {"album_id", album.at("id")}}).at("photos");
    REQUIRE(restored.size() == original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        CHECK(restored[i].at("id") == original[i].at("id"));
        CHECK(restored[i].at("path") == original[i].at("path"));
        CHECK(restored[i].at("availability") == "online");
        CHECK(restored[i].at("width") == original[i].at("width"));
        CHECK(restored[i].at("height") == original[i].at("height"));
        CHECK(restored[i].at("description") == original[i].at("description"));
        CHECK(restored[i].at("rating") == original[i].at("rating"));
    }
    CHECK(catalog.request({{"op", "metadata_pending"}}).at("metadata").empty());
    CHECK(std::filesystem::exists(source / "First.png"));
    CHECK(std::filesystem::exists(source / "Second.png"));
}

#endif
