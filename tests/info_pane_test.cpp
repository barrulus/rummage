#include "rummage/views/info_pane.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::views::Browser;
using rummage::views::InfoPane;
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

constexpr std::int32_t kWidth = 1200;
constexpr std::int32_t kHeight = 900;

// dir/{long.txt (200 lines), short.txt (5 lines)}, drawn offscreen.
struct Fixture {
    rummage::test::TempDir dir;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    Browser::Clock::time_point now = Browser::Clock::now();
    std::int32_t height{kHeight};

    Fixture() {
        std::ofstream longer{dir.path / "long.txt"};
        for (int i = 0; i < 200; ++i) {
            longer << "line " << i << " of a file that goes on\n";
        }
        longer.close();
        std::ofstream shorter{dir.path / "short.txt"};
        for (int i = 0; i < 5; ++i) {
            shorter << "short " << i << "\n";
        }
        shorter.close();
        browser.open(dir.path);
    }

    InfoPane& pane() { return browser.info_pane(); }

    // A frame, `later` after the last one: scrolls glide to where they were sent.
    void draw(std::chrono::milliseconds later = 0ms) {
        now += later;
        renderer.begin_frame(kWidth, height, 1.0F, {});
        browser.draw(renderer, {0.0F, 0.0F, kWidth, static_cast<float>(height)},
                     rummage::theme::Palette{}, icons, now);
        renderer.end_frame();
    }

    // Drawn until the pane has read what is selected.
    bool settle() {
        draw();
        const bool ok = run_until(loop, [this] { return !browser.loading(); });
        draw();
        return ok;
    }

    void press(xkb_keysym_t key, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = key;
        event.modifiers = modifiers;
        browser.key(event, now);
    }

    void pointer(rummage::ui::PointerEvent::Kind kind, float x, float y, int clicks = 1,
                 rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::PointerEvent event;
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.button = rummage::ui::PointerButton::left;
        event.click_count = clicks;
        event.modifiers = modifiers;
        browser.pointer(event, now);
    }

    void click(float x, float y, int clicks = 1, rummage::ui::Modifiers modifiers = {}) {
        pointer(rummage::ui::PointerEvent::Kind::press, x, y, clicks, modifiers);
        pointer(rummage::ui::PointerEvent::Kind::release, x, y);
    }

    void wheel(float x, float y, float notches) {
        rummage::ui::PointerEvent event;
        event.kind = rummage::ui::PointerEvent::Kind::scroll;
        event.x = x;
        event.y = y;
        event.wheel_y = notches;
        browser.pointer(event, now);
    }
};

// The offscreen context, or nullptr (and a message) on a machine without OpenGL.
std::unique_ptr<rummage::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<rummage::ui::Offscreen>(kWidth, kHeight);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

}  // namespace

TEST_CASE("info pane: compact media and copy include collapsed details without stale selections") {
    rummage::ui::Offscreen gl{kWidth, kHeight};
    Fixture f;
    const std::string value = "Embedded metadata " + std::string(200, 'x');
    const auto png = rummage::picture::encode_png(
        {.width = 1, .height = 1, .rgba = {10, 20, 30, 255}}, {{"Custom tag", value}});
    f.dir.write("metadata.png", {reinterpret_cast<const char*>(png.data()), png.size()});
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [](const auto& entry) { return entry.name == "metadata.png"; });
    }));
    folder.select_name("metadata.png");
    REQUIRE(folder.single_selection());
    REQUIRE(folder.single_selection()->name == "metadata.png");
    REQUIRE(run_until(f.loop, [&] {
        f.draw();
        return !f.pane().loading() && f.pane().summary() && f.pane().summary()->photo;
    }));
    std::string copied;
    f.pane().set_copy_handler([&](std::string text) { copied = std::move(text); });
    f.draw();
    CHECK_FALSE(f.pane().property_bounds("Dimensions.value").empty());
    CHECK(f.pane().property_bounds("Aspect ratio.value").empty());
    CHECK(f.pane().property_bounds("PNG · Custom tag.value").empty());
    const auto copy = f.pane().copy_bounds();
    REQUIRE_FALSE(copy.empty());
    CHECK(f.browser.cursor_at(copy.x + 4, copy.y + 4) == rummage::ui::CursorShape::pointer);
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("metadata.png") != std::string::npos);
    CHECK(copied.find("Dimensions: 1 × 1\n") != std::string::npos);
    CHECK(copied.find("PNG · Custom tag: " + value + "\n") != std::string::npos);
    CHECK(copied.find("Location: " + f.dir.path.string()) != std::string::npos);
    const auto all = copied;
    f.pane().set_expanded(true);
    f.draw();
    CHECK_FALSE(f.pane().property_bounds("PNG · Custom tag.value").empty());
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied == all);
    f.wheel(copy.x, copy.bottom() + 20, 20);
    f.draw(1s);
    CHECK(f.pane().copy_bounds().y == copy.y);
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied == all);
    folder.select_name("short.txt");
    f.draw();
    CHECK_FALSE(f.pane().click(copy.x + 4, copy.y + 4));
    CHECK(copied == all);
    REQUIRE(f.settle());
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("short.txt") != std::string::npos);
    CHECK(copied.find("PNG · Custom tag") == std::string::npos);
    f.pane().forget_layout();
    CHECK_FALSE(f.pane().click(copy.x + 4, copy.y + 4));
}

TEST_CASE("info pane: metadata text takes Ctrl+A/C while file selection stays intact") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    auto& folder = f.browser.columns().column(0).folder();
    const auto name = f.pane().property_bounds("name");
    REQUIRE_FALSE(name.empty());
    f.click(name.x + 2.0F, name.y + 2.0F);
    REQUIRE(f.pane().text_focused());
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(f.pane().selected_text() == "long.txt");
    CHECK(folder.selected_count() == 1);
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    CHECK(offered.front().second == "long.txt");

    const auto size = f.pane().property_bounds("Size.value");
    REQUIRE_FALSE(size.empty());
    f.click(size.x + 2.0F, size.y + 2.0F);
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK_FALSE(f.pane().selected_text().empty());
    CHECK(f.pane().selected_text() != "long.txt");
    f.press(XKB_KEY_c, {.ctrl = true});
    CHECK(offered.front().second == f.pane().selected_text());
    CHECK(folder.selected_count() == 1);

    // Pressing in the file view gives Ctrl+A and Ctrl+C back to file operations.
    f.click(400.0F, 300.0F);
    CHECK_FALSE(f.pane().text_focused());
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(folder.selected_count() == folder.entries().size());
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "x-special/gnome-copied-files");
}

TEST_CASE("info pane: a full long UTF-8 filename wraps and copies without visual line breaks") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    const std::string filename = std::string(160, 'a') + "-café-very-long-name.txt";
    std::ofstream{f.dir.path / filename} << "small file";
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [&](const auto& entry) { return entry.name == filename; });
    }));
    folder.select_name(filename);
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(f.settle());
    const auto name = f.pane().property_bounds("name");
    REQUIRE(name.height > 42.0F);
    CHECK(name.width <= f.pane().width());
    f.click(name.x + 2.0F, name.y + 2.0F, 3);
    CHECK(f.pane().selected_text() == filename);
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(f.pane().selected_text() == filename);
    // Reflow keeps the same source and selection.
    f.pane().set_width(InfoPane::kMaxWidth);
    f.draw();
    CHECK(f.pane().selected_text() == filename);
    CHECK(f.pane().property_bounds("name").height < name.height);
}

TEST_CASE("info pane: changing context or hiding it clears text selection and focus") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    const auto name = f.pane().property_bounds("name");
    f.click(name.x + 2.0F, name.y + 2.0F);
    f.press(XKB_KEY_a, {.ctrl = true});
    REQUIRE_FALSE(f.pane().selected_text().empty());
    SUBCASE("another selected file") { f.press(XKB_KEY_Down); }
    SUBCASE("hidden preview") { f.browser.set_info_visible(false); }
    SUBCASE("another tab") { f.browser.new_tab(); }
    SUBCASE("another pane") {
        f.browser.toggle_split();
        f.browser.switch_pane();
    }
    f.draw();
    CHECK_FALSE(f.pane().text_focused());
    CHECK(f.pane().selected_text().empty());
}

TEST_CASE("info pane: collapsing more details forgets selection of a hidden property") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_End);
    REQUIRE(f.settle());
    f.pane().set_expanded(true);
    f.draw();
    const auto location = f.pane().property_bounds("Location.value");
    REQUIRE_FALSE(location.empty());
    f.click(location.x + 2.0F, location.y + 2.0F);
    f.press(XKB_KEY_a, {.ctrl = true});
    REQUIRE(f.pane().selected_text() == f.dir.path.string());
    f.pane().set_expanded(false);
    f.draw();
    CHECK_FALSE(f.pane().text_focused());
    CHECK(f.pane().selected_text().empty());
}

TEST_CASE("info pane: modifier keys keep text focus and file navigation drops it before drawing") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    const auto name = f.pane().property_bounds("name");
    f.click(name.x + 2.0F, name.y + 2.0F);
    f.press(XKB_KEY_a, {.ctrl = true});
    REQUIRE(f.pane().selected_text() == "long.txt");
    f.press(XKB_KEY_Control_L);
    REQUIRE(f.pane().text_focused());
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    CHECK(offered.front().second == "long.txt");
    // Both key presses arrive before the next frame can refresh the info pane's content.
    f.press(XKB_KEY_Down);
    CHECK_FALSE(f.pane().text_focused());
    f.press(XKB_KEY_c, {.ctrl = true});
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().first == "x-special/gnome-copied-files");
    CHECK(offered.front().second.ends_with("short.txt"));
}

TEST_CASE("info pane: moving to the next file keeps the summary before until the next is read") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    f.press(XKB_KEY_Down);  // short.txt
    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    const float size_row = f.pane().property_bounds("Size.value").y;

    // long.txt: its summary is read on a worker. Until it arrives the page of short.txt stays,
    // so the rows under it do not jump up (an icon in its place) and back for a frame.
    f.press(XKB_KEY_Up);
    f.draw();
    CHECK(f.pane().summary() == nullptr);
    CHECK(f.pane().thumbnail_bounds().empty());  // drawn only: nothing to point at or select
    CHECK(f.pane().property_bounds("Size.value").y == size_row);
    CHECK(f.pane().selected_text().empty());  // drawn only
    // Its rows are short.txt's: they cannot be selected (or copied) under long.txt's name.
    const auto name = f.pane().property_bounds("name");
    REQUIRE_FALSE(name.empty());
    f.click(name.x + 2.0F, name.y + 2.0F, 2);
    CHECK_FALSE(f.pane().text_focused());
    CHECK(f.pane().selected_text().empty());

    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    REQUIRE(f.pane().summary()->text);
    CHECK(f.pane().summary()->text->lines.size() == 12);
    CHECK(f.pane().scroll_offset() == 0.0F);
    CHECK_FALSE(f.pane().thumbnail_bounds().empty());
}

TEST_CASE("info pane: text is a bounded thumbnail and Space opens the readable Quick Look") {
    const auto gl = offscreen();
    if (!gl)
        return;
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    const auto* summary = f.pane().summary();
    REQUIRE(summary);
    REQUIRE(summary->text);
    CHECK(summary->text->lines.size() == 12);
    CHECK(summary->text->lines.front() == "line 0 of a file that goes on");
    CHECK(summary->text->truncated);
    const auto box = f.pane().thumbnail_bounds();
    REQUIRE_FALSE(box.empty());
    CHECK(box.height <= 12 * InfoPane::kTextLineHeight + 16);
    f.wheel(box.x + 10, box.y + 10, 3);
    f.draw(1000ms);
    CHECK_FALSE(f.pane().text_focused());
    CHECK(f.pane().selected_text().empty());
    CHECK_FALSE(f.pane().property_bounds("preview-hint").empty());
    f.press(XKB_KEY_space);
    REQUIRE(f.settle());
    REQUIRE(f.browser.quick_look().is_open());
    const auto* content = f.browser.quick_look().content();
    REQUIRE(content);
    const auto* text = std::get_if<rummage::preview::Text>(content);
    REQUIRE(text);
    CHECK(text->lines.size() == 200);
    CHECK(text->lines.back() == "line 199 of a file that goes on");
}

TEST_CASE("info pane: applying permissions refreshes cached metadata without changing mtime") {
    const auto gl = offscreen();
    if (!gl)
        return;
    Fixture f;
    REQUIRE(::chmod((f.dir.path / "long.txt").c_str(), 0644) == 0);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    CHECK(f.pane().summary()->info.permissions == "-rw-r--r--");
    f.browser.show_properties();
    REQUIRE(f.settle());
    auto& properties = f.browser.properties_dialog();
    properties.set_permissions(0600);
    properties.apply();
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    CHECK(f.pane().summary()->info.permissions == "-rw-------");
}

TEST_CASE("info pane: source summary is scoped to its folder and opens language insights") {
    rummage::ui::Offscreen offscreen{kWidth, kHeight};
    Fixture f;
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(f.settle());
    fs::create_directory(f.dir.path / "source");
    auto& folder = *f.browser.picker_folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [](const auto& entry) { return entry.name == "source"; });
    }));
    folder.select_name("source");
    REQUIRE(f.settle());
    auto report = std::make_shared<rummage::platform::CodeReport>();
    report->root = f.dir.path / "source";
    report->files = 2;
    report->total = {17, 8, 3};
    f.pane().set_code_report(report);
    std::string copied;
    f.pane().set_copy_handler([&](std::string text) { copied = std::move(text); });
    f.draw();
    if (const char* directory = std::getenv("RUMMAGE_INSIGHTS_FRAMES")) {
        std::vector<unsigned char> pixels(static_cast<std::size_t>(kWidth * kHeight * 4));
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream image{fs::path{directory} / "info-source.ppm", std::ios::binary};
        image << "P6\n" << kWidth << " " << kHeight << "\n255\n";
        for (int y = kHeight - 1; y >= 0; --y)
            for (int x = 0; x < kWidth; ++x)
                image.write(reinterpret_cast<const char*>(pixels.data() + (y * kWidth + x) * 4), 3);
        REQUIRE(image.good());
    }
    auto copy = f.pane().copy_bounds();
    REQUIRE_FALSE(copy.empty());
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Source files: 2\n") != std::string::npos);
    CHECK(copied.find("Lines: 28\n") != std::string::npos);
    CHECK(copied.find("Code: 17\n") != std::string::npos);
    auto button = f.pane().code_bounds();
    REQUIRE_FALSE(button.empty());
    f.click(button.x + 4, button.y + 4);
    CHECK(f.browser.insights_view().is_open());
    CHECK(f.browser.insights_view().view() == rummage::views::InsightsView::View::languages);
    REQUIRE(f.browser.tab_count() == 2);
    f.browser.close_tab(f.browser.active_tab());
    f.browser.picker_folder()->select_name("short.txt");
    REQUIRE(f.settle());
    CHECK(f.pane().code_bounds().empty());
    copy = f.pane().copy_bounds();
    f.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Source files:") == std::string::npos);
}

TEST_CASE("info pane: preview follows focus independently of the selected set in every view") {
    const auto gl = offscreen();
    if (!gl)
        return;
    Fixture f;
    REQUIRE(f.settle());
    SUBCASE("columns") { f.press(XKB_KEY_1, {.ctrl = true}); }
    SUBCASE("grid") { f.press(XKB_KEY_2, {.ctrl = true}); }
    SUBCASE("details") { f.press(XKB_KEY_3, {.ctrl = true}); }
    const auto folder = f.browser.picker_folder();
    REQUIRE(folder);
    folder->set_cursor(0);
    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    REQUIRE(f.pane().summary()->text);
    CHECK(f.pane().summary()->text->lines.size() == 12);
    folder->set_cursor(1, rummage::views::SelectMode::extend);
    REQUIRE(f.settle());
    CHECK(folder->selected_count() == 2);
    REQUIRE(f.pane().summary()->text);
    CHECK(f.pane().summary()->text->lines.size() == 5);
    CHECK_FALSE(f.browser.selection_count_bounds().empty());
    CHECK_FALSE(f.pane().property_bounds("name").empty());

    // Toggle the focused file out of the selection; its preview stays visible.
    const auto* summary = f.pane().summary();
    folder->set_cursor(1, rummage::views::SelectMode::toggle);
    f.draw();
    CHECK(folder->selected_count() == 1);
    CHECK_FALSE(folder->is_selected("short.txt"));
    CHECK(f.pane().summary() == summary);
    folder->move_cursor(-1, rummage::views::SelectMode::keep);
    REQUIRE(f.settle());
    CHECK(folder->selected_names() == std::vector<std::string>{"long.txt"});
    CHECK(f.pane().summary()->text->lines.size() == 12);
    folder->select_all();
    f.draw();
    CHECK(folder->selected_count() == 2);
    folder->invert_selection();
    f.draw();
    CHECK(folder->selected_count() == 0);
    CHECK(f.browser.selection_count_bounds().empty());
    CHECK_FALSE(f.pane().property_bounds("name").empty());

    // An explicit preview hide does not hide the count or change selection.
    folder->select_all();
    f.browser.set_info_visible(false);
    f.draw();
    CHECK_FALSE(f.browser.selection_count_bounds().empty());
    CHECK(folder->selected_count() == 2);
}

TEST_CASE("info pane: automatic visibility follows focus and manual Show overrides it") {
    const auto gl = offscreen();
    if (!gl)
        return;
    Fixture f;
    REQUIRE(f.settle());
    SUBCASE("grid") { f.press(XKB_KEY_2, {.ctrl = true}); }
    SUBCASE("details") { f.press(XKB_KEY_3, {.ctrl = true}); }
    SUBCASE("split") { f.browser.toggle_split(); }
    SUBCASE("sidebar") { f.browser.set_preview_in_sidebar(true); }
    REQUIRE(f.settle());
    f.browser.set_info_auto_hide(true);
    const auto folder = f.browser.picker_folder();
    REQUIRE(folder);
    folder->set_cursor(std::nullopt);
    f.draw();
    CHECK_FALSE(f.browser.info_shown());
    CHECK(f.pane().copy_bounds().empty());
    folder->set_cursor(0);
    REQUIRE(f.settle());
    CHECK(f.browser.info_shown());
    CHECK_FALSE(f.pane().copy_bounds().empty());
    folder->select_all();
    f.draw();
    CHECK(f.browser.info_shown());
    folder->set_cursor(std::nullopt);
    f.draw();
    CHECK_FALSE(f.browser.info_shown());
    f.press(XKB_KEY_i, {.ctrl = true});
    f.draw();
    CHECK(f.browser.info_shown());
    CHECK_FALSE(f.browser.info_auto_hide());
    f.browser.set_info_auto_hide(true);
    f.browser.set_info_visible(false);
    folder->set_cursor(0);
    f.draw();
    CHECK_FALSE(f.browser.info_shown());
    const auto state = f.browser.state();
    CHECK(state.info_auto_hide);
    CHECK_FALSE(state.info_visible);
}

TEST_CASE("info pane: automatic hiding handles metadata-only files deletion and picker mode") {
    const auto gl = offscreen();
    if (!gl)
        return;
    Fixture f;
    REQUIRE(f.settle());
    f.browser.set_mode(Browser::Mode::grid);
    SUBCASE("browser") {}
    SUBCASE("picker") { f.browser.set_picker_mode(true); }
    const auto path = f.dir.path / "unknown.binary";
    std::ofstream(path).write("\0\1\2", 3);
    const auto folder = f.browser.picker_folder();
    folder->expect_name("unknown.binary");
    REQUIRE(run_until(f.loop, [&] { return folder->cursor_entry() != nullptr; }));
    REQUIRE(f.settle());
    REQUIRE(folder->cursor_entry());
    REQUIRE(folder->cursor_entry()->name == "unknown.binary");
    f.browser.set_info_auto_hide(true);
    REQUIRE(f.settle());
    CHECK(f.browser.info_shown());
    CHECK_FALSE(f.pane().property_bounds("name").empty());
    fs::remove(path);
    REQUIRE(run_until(f.loop, [&] {
        const auto* entry = folder->cursor_entry();
        return !entry || entry->name != "unknown.binary";
    }));
    REQUIRE(f.settle());
    folder->set_cursor(std::nullopt);
    f.draw();
    CHECK_FALSE(f.browser.info_shown());
    CHECK(f.pane().copy_bounds().empty());
    CHECK_FALSE(f.pane().loading());
}

TEST_CASE("info pane: collection metadata stays cached for offline originals and empty roots") {
    const auto gl = offscreen();
    if (!gl)
        return;
    EventLoop loop;
    WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    rummage::views::Folder collection{loop, pool, watcher, mime, [] {}};
    InfoPane pane{loop, pool, nullptr, [] {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    std::string copied;
    pane.set_copy_handler([&](std::string value) { copied = std::move(value); });
    const auto draw = [&] {
        renderer.begin_frame(400, 900, 1, {});
        pane.draw(renderer, {0, 0, 400, 900}, rummage::theme::Palette{}, icons, collection,
                  InfoPane::Clock::now());
        renderer.end_frame();
    };
    collection.replace_collection({}, "Photo Library", 0);
    draw();
    CHECK_FALSE(pane.loading());
    CHECK(pane.summary() == nullptr);
    rummage::test::TempDir directory;
    rummage::fs::Entry entry;
    entry.name = "photo.jpg";
    entry.identity = "photo:71";
    entry.location = directory.path / "offline" / entry.name;
    entry.available = false;
    entry.kind = rummage::fs::EntryKind::file;
    entry.size = 2048;
    collection.replace_collection({entry}, "Photo Library", 1);
    collection.select_name(entry.key());
    rummage::views::PhotoProperties properties;
    properties.ids = {71};
    properties.path = entry.location;
    properties.entry = entry;
    properties.available = false;
    properties.rows = {{"Availability", "Offline"}, {"Camera", "Cached camera"}};
    properties.annotations.description = "A café";
    properties.annotations.rating = 4;
    properties.annotations.tags = std::vector<std::string>{"東京", "family"};
    pane.set_photo_properties(properties);
    draw();
    CHECK_FALSE(pane.loading());
    CHECK(pane.summary() == nullptr);
    CHECK_FALSE(pane.property_bounds("Camera.value").empty());
    const auto copy = pane.copy_bounds();
    REQUIRE_FALSE(copy.empty());
    pane.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Cached camera") != std::string::npos);
    CHECK(copied.find("A café") != std::string::npos);
    CHECK(copied.find("東京, family") != std::string::npos);
    CHECK_FALSE(fs::exists(entry.location.parent_path()));
    // A same-name different identity must not inherit the preceding item's catalog fields.
    entry.identity = "photo:72";
    entry.location = directory.path / "elsewhere" / entry.name;
    collection.replace_collection({entry}, "Photo Library", 1);
    collection.select_name(entry.key());
    draw();
    pane.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Cached camera") == std::string::npos);
    CHECK_FALSE(pane.loading());
}

TEST_CASE("info pane: logical collection records use stable identity without file metadata") {
    const auto gl = offscreen();
    if (!gl)
        return;
    EventLoop loop;
    WorkerPool pool{loop, 1};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto mime = rummage::fs::MimeDatabase::load();
    rummage::views::Folder collection{loop, pool, watcher, mime, [] {}};
    InfoPane pane{loop, pool, nullptr, [] {}};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    std::string copied;
    pane.set_copy_handler([&](std::string value) { copied = std::move(value); });
    const auto draw = [&] {
        renderer.begin_frame(400, 900, 1, {});
        pane.draw(renderer, {0, 0, 400, 900}, rummage::theme::Palette{}, icons, collection,
                  InfoPane::Clock::now());
        renderer.end_frame();
    };
    rummage::fs::Entry entry;
    entry.name = "Shared title";
    entry.identity = "job:19";
    entry.kind = rummage::fs::EntryKind::directory;
    entry.available = false;
    collection.replace_collection({entry}, "Activity", 1);
    collection.select_name(entry.key());
    rummage::views::PhotoProperties properties;
    properties.entry = entry;
    properties.title = entry.name;
    properties.available = false;
    properties.rows = {{"State", "Paused"}, {"Completed", "42"}, {"Total", "100"}};
    pane.set_photo_properties(properties);
    draw();
    CHECK_FALSE(pane.loading());
    CHECK(pane.summary() == nullptr);
    CHECK_FALSE(pane.property_bounds("State.value").empty());
    CHECK(pane.property_bounds("Modified.value").empty());
    CHECK(pane.property_bounds("Size.value").empty());
    CHECK(pane.property_bounds("Description.value").empty());
    const auto copy = pane.copy_bounds();
    pane.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Paused") != std::string::npos);
    CHECK(copied.find("Mixed values") == std::string::npos);
    // Empty paths and equal titles cannot make two records interchangeable.
    entry.identity = "album:19";
    collection.replace_collection({entry}, "Albums", 1);
    collection.select_name(entry.key());
    draw();
    pane.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Paused") == std::string::npos);
    properties.entry = entry;
    properties.rows = {{"Name", "Shared title"}, {"Photo count", "9"}};
    pane.set_photo_properties(properties);
    draw();
    pane.click(copy.x + 4, copy.y + 4);
    CHECK(copied.find("Photo count: 9") != std::string::npos);
    CHECK_FALSE(pane.loading());
}
