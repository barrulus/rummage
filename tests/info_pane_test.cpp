#include "rummage/views/info_pane.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
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
#include "rummage/views/decorations.hpp"
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

TEST_CASE("info pane: a long text file's page scrolls with the wheel over it, not elsewhere") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    InfoPane& pane = f.pane();
    REQUIRE(pane.text_page().width > 0.0F);
    CHECK(pane.text_max_offset() > 0.0F);
    CHECK(pane.text_offset() == 0.0F);
    const auto page = pane.text_page();
    const float inside_x = page.x + (page.width / 2.0F);
    // Beside the page, still on the pane (the name and the rows under it): the pane scrolls.
    f.wheel(inside_x, page.bottom() + 10.0F, 3.0F);
    f.draw(1000ms);
    CHECK(pane.text_offset() == 0.0F);

    // Over it: the page. Three notches of 48 logical pixels.
    f.wheel(inside_x, page.y + (page.height / 2.0F), 3.0F);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(144.0F).epsilon(0.01));
    CHECK(pane.text_offset() <= pane.text_max_offset());
}

TEST_CASE("info pane: Alt+J and Alt+K move the page three lines, Alt+PgDn and Alt+PgUp a page") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    InfoPane& pane = f.pane();
    REQUIRE(pane.text_max_offset() > 0.0F);
    const rummage::ui::Modifiers alt{.alt = true};
    const float line = InfoPane::kTextLineHeight;

    f.press(XKB_KEY_j, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(3.0F * line).epsilon(0.01));
    f.press(XKB_KEY_J, alt);  // Caps Lock
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(6.0F * line).epsilon(0.01));
    f.press(XKB_KEY_k, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(3.0F * line).epsilon(0.01));

    // A page: what the page shows less a line.
    const float before = pane.text_offset();
    f.press(XKB_KEY_Page_Down, alt);
    f.draw(1000ms);
    const float page = pane.text_offset() - before;
    CHECK(page > 10.0F * line);
    CHECK(page < pane.text_page().height);
    f.press(XKB_KEY_Page_Up, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(before).epsilon(0.01));

    // Past the top: stays at it.
    f.press(XKB_KEY_Page_Up, alt);
    f.press(XKB_KEY_Page_Up, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == 0.0F);
    // And the end.
    for (int i = 0; i < 20; ++i) {
        f.press(XKB_KEY_Page_Down, alt);
    }
    f.draw(2000ms);
    CHECK(pane.text_offset() == pane.text_max_offset());
}

TEST_CASE(
    "info pane: the keys work in a file picker, which has no panes, and not under Quick Look") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    f.browser.set_panes_enabled(false);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    InfoPane& pane = f.pane();
    const rummage::ui::Modifiers alt{.alt = true};
    f.press(XKB_KEY_j, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(3.0F * InfoPane::kTextLineHeight).epsilon(0.01));

    // Quick Look covers the pane: the keys are not for it.
    f.press(XKB_KEY_space);
    REQUIRE(f.browser.quick_look().is_open());
    f.press(XKB_KEY_j, alt);
    f.press(XKB_KEY_Page_Down, alt);
    f.draw(1000ms);
    CHECK(pane.text_offset() == doctest::Approx(3.0F * InfoPane::kTextLineHeight).epsilon(0.01));
}

TEST_CASE("info pane: another file starts at the top, and a short one has nothing to scroll") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    InfoPane& pane = f.pane();
    const rummage::ui::Modifiers alt{.alt = true};
    f.press(XKB_KEY_j, alt);
    f.draw(1000ms);
    REQUIRE(pane.text_offset() > 0.0F);

    f.press(XKB_KEY_Down);  // short.txt: five lines fit the page
    REQUIRE(f.settle());
    REQUIRE(pane.text_page().width > 0.0F);
    CHECK(pane.text_offset() == 0.0F);
    CHECK(pane.text_max_offset() == 0.0F);
    f.press(XKB_KEY_j, alt);
    f.wheel(pane.text_page().x + 10.0F, pane.text_page().y + 10.0F, 3.0F);
    f.draw(1000ms);
    CHECK(pane.text_offset() == 0.0F);

    f.press(XKB_KEY_Up);  // back to long.txt: at the top, not where it was left
    REQUIRE(f.settle());
    CHECK(pane.text_max_offset() > 0.0F);
    CHECK(pane.text_offset() == 0.0F);
}

TEST_CASE("info pane: a log larger than what is read opens at its newest lines") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    {
        std::ofstream log{f.dir.path / "a.log"};
        // More than is read (256 KiB): only then is what is shown the log's end.
        for (int i = 0; i < 12000; ++i) {
            log << "2026-11-13 12:00:" << i << " something happened\n";
        }
    }
    // The initial scan may have completed before the log was created.
    REQUIRE(run_until(f.loop, [&] {
        const auto& entries = f.browser.columns().column(0).folder().entries();
        return std::ranges::any_of(
            entries, [](const rummage::fs::Entry& entry) { return entry.name == "a.log"; });
    }));
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // a.log, before long.txt
    REQUIRE(f.settle());
    const auto* selected = f.browser.columns().column(0).folder().cursor_entry();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->name == "a.log");
    InfoPane& pane = f.pane();
    // The log is read off the main thread; under load it can arrive after the folder settles.
    REQUIRE(run_until(f.loop, [&] {
        f.draw();
        return pane.text_page().width > 0.0F;
    }));
    REQUIRE(pane.text_max_offset() > 0.0F);
    CHECK(pane.text_offset() == pane.text_max_offset());
}

TEST_CASE("info pane: replacing or hiding a scrolling text page stops redraws") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    f.press(XKB_KEY_j, {.alt = true});
    f.draw(16ms);
    REQUIRE(f.pane().animating());
    SUBCASE("multiple selection") { f.press(XKB_KEY_a, {.ctrl = true}); }
    SUBCASE("hidden pane") { f.browser.set_info_visible(false); }
    f.draw(16ms);
    REQUIRE(f.pane().text_page().width == 0.0F);
    CHECK_FALSE(f.pane().animating());
    f.draw(1000ms);
    CHECK_FALSE(f.browser.animating());
}

TEST_CASE("info pane: text pages fit short windows and reach their final line") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    SUBCASE("minimum window height") { f.height = 320; }
    SUBCASE("short window") { f.height = 400; }
    SUBCASE("default window height") { f.height = 640; }
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);  // long.txt
    REQUIRE(f.settle());
    const auto page = f.pane().text_page();
    REQUIRE(page.width > 0.0F);
    CHECK(page.y >= 0.0F);
    CHECK(page.bottom() <= static_cast<float>(f.height));

    f.press(XKB_KEY_Page_Down, {.alt = true});
    f.draw(1000ms);
    CHECK(f.pane().text_offset() > 0.0F);
    CHECK(f.pane().text_offset() < page.height);
    CHECK(f.pane().text_page().y == page.y);  // the outer pane did not move
    for (int i = 0; i < 100; ++i) {
        f.press(XKB_KEY_Page_Down, {.alt = true});
    }
    f.draw(2000ms);
    CHECK(f.pane().text_offset() == f.pane().text_max_offset());
    REQUIRE(f.pane().summary() != nullptr);
    REQUIRE(f.pane().summary()->text.has_value());
    const float content =
        static_cast<float>(f.pane().summary()->text->lines.size()) * InfoPane::kTextLineHeight;
    const float viewport = content - f.pane().text_max_offset();
    CHECK(viewport < page.height);
    CHECK(viewport >= InfoPane::kTextLineHeight);

    // Resizing to a tall window expands the page and clamps the offset at the new end.
    const float previous_max = f.pane().text_max_offset();
    f.height = kHeight;
    f.draw(1000ms);
    CHECK(f.pane().text_max_offset() <= previous_max);
    CHECK(f.pane().text_offset() == f.pane().text_max_offset());
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

TEST_CASE("info pane: preview text supports words, lines, drag, shift and loaded-text selection") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    REQUIRE(f.settle());
    const auto page = f.pane().text_page();
    REQUIRE_FALSE(page.empty());
    using Kind = rummage::ui::PointerEvent::Kind;
    const float x = page.x + 10.0F;
    const float y = page.y + 10.0F;
    f.click(x, y, 2);
    CHECK(f.pane().selected_text() == "line");
    f.click(x, y, 3);
    CHECK(f.pane().selected_text() == "line 0 of a file that goes on\n");
    f.pointer(Kind::press, x, y);
    f.pointer(Kind::motion, x + 30.0F, y);
    f.pointer(Kind::release, x + 30.0F, y);
    const auto dragged = f.pane().selected_text();
    CHECK_FALSE(dragged.empty());
    f.click(x + 60.0F, y, 1, {.shift = true});
    CHECK(f.pane().selected_text().size() > dragged.size());
    f.press(XKB_KEY_a, {.ctrl = true});
    CHECK(f.pane().selected_text().starts_with("line 0"));
    CHECK(f.pane().selected_text().ends_with("line 199 of a file that goes on"));
    f.press(XKB_KEY_End);
    f.draw(1000ms);
    CHECK(f.pane().text_offset() == f.pane().text_max_offset());
    CHECK(f.pane().text_focused());
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

TEST_CASE(
    "info pane: shaped Unicode clusters select accurately and long-line tails are reachable") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    const std::string first = "漢字 é 👩🏽‍💻 tail";
    std::string long_line;
    for (int i = 0; i < 150; ++i) {
        long_line += "漢字";
    }
    long_line += " END";
    std::ofstream{f.dir.path / "unicode.txt"} << first << "\n" << long_line << "\n";
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [](const auto& entry) { return entry.name == "unicode.txt"; });
    }));
    folder.select_name("unicode.txt");
    REQUIRE(f.settle());
    const auto page = f.pane().text_page();
    REQUIRE_FALSE(page.empty());
    constexpr float size = 11.0F;
    const auto first_layout = f.fonts.layout(first, size, 1.0F, 0.0F, rummage::ui::Ellipsis::end,
                                             rummage::ui::Typeface::monospace);
    const auto combining = std::ranges::find_if(
        first_layout.glyphs, [&](const auto& glyph) { return glyph.cluster == first.find('e'); });
    REQUIRE(combining != first_layout.glyphs.end());
    const float combining_x = page.x + 8.0F + combining->x + combining->advance * 0.4F;
    f.click(combining_x, page.y + 10.0F, 2);
    CHECK(f.pane().selected_text() == "é");

    const auto long_layout = f.fonts.layout(long_line, size, 1.0F, 0.0F, rummage::ui::Ellipsis::end,
                                            rummage::ui::Typeface::monospace);
    CHECK(f.pane().text_horizontal_max_offset() >= long_layout.width - (page.width - 16.0F));
    rummage::ui::PointerEvent wheel;
    wheel.kind = rummage::ui::PointerEvent::Kind::scroll;
    wheel.x = page.x + page.width / 2.0F;
    wheel.y = page.y + 10.0F;
    wheel.wheel_y = 1000.0F;
    wheel.modifiers.shift = true;
    f.browser.pointer(wheel, f.now);
    f.draw(2000ms);
    CHECK(f.pane().text_horizontal_offset() == f.pane().text_horizontal_max_offset());
    const auto tail = std::ranges::find_if(long_layout.glyphs, [&](const auto& glyph) {
        return glyph.cluster == long_line.find("END");
    });
    REQUIRE(tail != long_layout.glyphs.end());
    const float tail_x =
        page.x + 8.0F + tail->x + tail->advance * 0.4F - f.pane().text_horizontal_offset();
    REQUIRE(tail_x > page.x);
    REQUIRE(tail_x < page.right());
    f.click(tail_x, page.y + 10.0F + InfoPane::kTextLineHeight, 2);
    CHECK(f.pane().selected_text() == "END");
}

TEST_CASE("info pane: large previews retain selection at the end of the document") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    {
        std::ofstream source{f.dir.path / "large.c"};
        for (int i = 0; i < 5000; ++i) {
            source << "int variable_" << i << " = " << i << ";\n";
        }
        source << "last_line\n";
    }
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [](const auto& entry) { return entry.name == "large.c"; });
    }));
    folder.select_name("large.c");
    REQUIRE(f.settle());
    REQUIRE(f.pane().summary());
    REQUIRE(f.pane().summary()->text);
    REQUIRE(f.pane().summary()->text->lines.size() == 5001);
    CHECK(f.pane().text_geometry_lines() == 0);
    const auto page = f.pane().text_page();
    REQUIRE_FALSE(page.empty());
    f.click(page.x + 12.0F, page.y + 10.0F);
    f.press(XKB_KEY_End);
    f.draw(2000ms);
    CHECK(f.pane().text_offset() == f.pane().text_max_offset());
    const float last_y =
        page.y + 8.0F + 5000.0F * InfoPane::kTextLineHeight - f.pane().text_offset() + 2.0F;
    REQUIRE(last_y < page.bottom());
    f.click(page.x + 12.0F, last_y, 2);
    CHECK(f.pane().selected_text() == "last_line");
    CHECK(f.pane().text_geometry_lines() <= 64);

    // Selecting and painting many pages must not keep geometry for every visited line.
    f.press(XKB_KEY_a, {.ctrl = true});
    for (int i = 0; i < 8; ++i) {
        f.press(XKB_KEY_Page_Up);
        f.draw(1000ms);
        CHECK(f.pane().text_geometry_lines() <= 64);
    }
    CHECK(f.pane().selected_text().ends_with("last_line"));

    // Hit testing remains exact even outside the painted page during a drag.
    f.press(XKB_KEY_Home);
    f.draw(1000ms);
    f.pointer(rummage::ui::PointerEvent::Kind::press, page.x + 8.0F, page.y + 10.0F);
    f.pointer(rummage::ui::PointerEvent::Kind::release, page.right() + 100.0F,
              page.y + 8.0F + 100.0F * InfoPane::kTextLineHeight + 2.0F);
    CHECK(f.pane().selected_text().ends_with("int variable_100 = 100;"));
    CHECK(f.pane().text_geometry_lines() <= 64);
}

TEST_CASE("info pane: a short wide preview pans sideways but vertical wheel scrolls its metadata") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    f.height = 320;
    std::ofstream{f.dir.path / "wide.txt"} << std::string(600, 'w') << "\n";
    auto& folder = f.browser.columns().column(0).folder();
    REQUIRE(run_until(f.loop, [&] {
        return std::ranges::any_of(folder.entries(),
                                   [](const auto& entry) { return entry.name == "wide.txt"; });
    }));
    folder.select_name("wide.txt");
    f.pane().set_expanded(true);
    REQUIRE(f.settle());
    const auto page = f.pane().text_page();
    REQUIRE_FALSE(page.empty());
    REQUIRE(f.pane().text_max_offset() == 0.0F);
    REQUIRE(f.pane().text_horizontal_max_offset() > 0.0F);
    const auto name = f.pane().property_bounds("name");
    rummage::ui::PointerEvent horizontal;
    horizontal.kind = rummage::ui::PointerEvent::Kind::scroll;
    horizontal.x = page.x + page.width / 2.0F;
    horizontal.y = page.y + 10.0F;
    horizontal.wheel_y = 2.0F;
    horizontal.modifiers.shift = true;
    f.browser.pointer(horizontal, f.now);
    f.draw(1000ms);
    CHECK(f.pane().text_horizontal_offset() > 0.0F);
    CHECK(f.pane().property_bounds("name").y == name.y);
    f.wheel(horizontal.x, horizontal.y, 3.0F);
    f.draw(1000ms);
    CHECK(f.pane().text_offset() == 0.0F);
    CHECK(f.pane().property_bounds("name").y < name.y);
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

namespace {

// A decorator with one section: a link row and nothing else.
struct LinkDecorations final : rummage::views::Decorations {
    int opened{0};
    rummage::views::DetailSection section;

    LinkDecorations() {
        section.title = "EXTRA";
        section.rows.push_back({.label = "Link",
                                .value = "example.org/a/long/enough/path",
                                .action = [this] { ++opened; }});
    }

    const rummage::views::Badge* badge(const rummage::views::Folder& /*folder*/,
                                       const rummage::fs::Entry& /*entry*/) override {
        return nullptr;
    }

    const rummage::views::StatusPart* status(const rummage::views::Folder& /*folder*/) override {
        return nullptr;
    }

    const rummage::views::DetailSection* details(
        const rummage::views::Folder& /*folder*/) override {
        return &section;
    }
};

}  // namespace

TEST_CASE("info pane: a decorator's link is clicked, not selected as text") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    Fixture f;
    LinkDecorations decorations;
    f.browser.set_decorations(&decorations);
    REQUIRE(f.settle());
    f.press(XKB_KEY_Down);
    f.press(XKB_KEY_Down);  // short.txt: its short page leaves room for the section after it
    REQUIRE(f.settle());
    // The section's heading is clickable across the pane; under it, the link only at the right.
    float heading_left = 0.0F;
    float heading_right = 0.0F;
    float heading_y = -1.0F;
    for (float y = 0.0F; y < static_cast<float>(kHeight) && heading_y < 0.0F; y += 2.0F) {
        for (float x = 0.0F; x < static_cast<float>(kWidth); x += 2.0F) {
            if (f.pane().clickable_at(x, y)) {
                if (heading_y < 0.0F) {
                    heading_y = y;
                    heading_left = x;
                }
                heading_right = x;
            }
        }
        if (heading_right - heading_left < 200.0F) {
            heading_y = -1.0F;  // "More details", narrow: not the heading
        }
    }
    REQUIRE(heading_y >= 0.0F);
    const float left = heading_left + 10.0F;
    const float right = heading_right - 10.0F;
    std::optional<float> link_y;
    for (float y = heading_y + 30.0F; y < static_cast<float>(kHeight) && !link_y; y += 2.0F) {
        if (f.pane().clickable_at(right, y) && !f.pane().clickable_at(left, y)) {
            link_y = y;
        }
    }
    REQUIRE(link_y.has_value());
    f.click(right, *link_y + 2.0F);
    CHECK(decorations.opened == 1);
    f.browser.set_decorations(nullptr);
}
