#include "rummage/ui/motion.hpp"

#include <GLES3/gl3.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/platform/portal.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

namespace core = rummage::core;
namespace ui = rummage::ui;
using namespace std::chrono_literals;
using Event = core::MotionEvent;
using Effect = core::MotionEffect;
using Channel = ui::MotionChannel;

TEST_CASE("motion: bounded channels, replacement, cancellation and monotonic easing") {
    ui::Motion motion;
    const ui::Motion::Clock::time_point now{};
    CHECK_FALSE(motion.active());
    REQUIRE(motion.start(Channel::first_pane, Event::file_copy, now, {0, 0, 32, 32},
                         {100, 80, 16, 16}));
    const auto halfway = motion.frame(Channel::first_pane, now + 210ms);
    REQUIRE(halfway);
    CHECK(halfway->progress == doctest::Approx(0.5));
    CHECK(halfway->geometry().x == doctest::Approx(87.5));
    CHECK(motion.frame(Channel::first_pane, now - 1s)->progress == 0);
    for (int i = 1; i <= 100; ++i)
        CHECK(ui::Motion::ease(static_cast<float>(i) / 100) >=
              ui::Motion::ease(static_cast<float>(i - 1) / 100));
    REQUIRE(motion.start(Channel::first_pane, Event::directory_back, now + 10ms));
    CHECK(motion.frame(Channel::first_pane, now + 10ms)->effect == Effect::ripple);
    for (const auto channel : ui::kMotionChannels)
        REQUIRE(motion.start(channel, Event::file_move, now));
    CHECK_FALSE(motion.start(Channel::count, Event::file_copy, now));
    motion.cancel(Channel::first_pane);
    CHECK_FALSE(motion.frame(Channel::first_pane, now));
    for (const auto channel : {Channel::second_pane, Channel::overlay, Channel::operation}) {
        REQUIRE(motion.frame(channel, now));
        CHECK(motion.frame(channel, now)->effect == Effect::portal);
    }
    CHECK(ui::pane_motion_channel(0) == Channel::first_pane);
    CHECK(ui::pane_motion_channel(1) == Channel::second_pane);
    CHECK_FALSE(motion.frame(ui::pane_motion_channel(2), now));
    motion.advance(now + 2s);
    CHECK_FALSE(motion.active());
}

TEST_CASE("motion: item bounds exclude offscreen selections and reset between draws") {
    rummage::views::ItemBounds bounds;
    const ui::Rect viewport{10, 20, 100, 80};
    bounds.include({0, 10, 40, 40}, viewport, true, true);
    CHECK(bounds.cursor.x == 10);
    CHECK(bounds.cursor.y == 20);
    CHECK(bounds.cursor.width == 30);
    CHECK(bounds.cursor.height == 30);
    bounds.include({80, 80, 60, 60}, viewport, false, true);
    bounds.include({500, 500, 20, 20}, viewport, false, true);
    bounds.include({10, 20, 100, 80}, viewport, false, false);
    CHECK(bounds.selection.x == 10);
    CHECK(bounds.selection.y == 20);
    CHECK(bounds.selection.width == 100);
    CHECK(bounds.selection.height == 80);
    CHECK(bounds.cursor.width == 30);
    bounds = {};
    bounds.include({500, 500, 20, 20}, viewport, true, true);
    CHECK(bounds.cursor.empty());
    CHECK(bounds.selection.empty());
}

TEST_CASE("motion: disabled and reduced modes cancel immediately and never schedule") {
    ui::Motion motion;
    const auto now = ui::Motion::Clock::now();
    core::MotionConfig config;
    REQUIRE(motion.start(Channel::first_pane, Event::directory_open, now));
    motion.set_system_reduced(true);
    CHECK_FALSE(motion.active());
    CHECK_FALSE(motion.start(Channel::first_pane, Event::directory_open, now));
    config.reduce_motion = core::MotionConfig::Reduce::never;
    motion.configure(config);
    REQUIRE(motion.start(Channel::first_pane, Event::directory_open, now));
    config.enabled = false;
    motion.configure(config);
    CHECK_FALSE(motion.active());
    CHECK_FALSE(motion.start(Channel::first_pane, Event::directory_open, now));
    config.enabled = true;
    config.duration_scale = 0;
    motion.configure(config);
    CHECK_FALSE(motion.start(Channel::first_pane, Event::directory_open, now));
    config.duration_scale = std::numeric_limits<float>::quiet_NaN();
    motion.configure(config);
    CHECK_FALSE(motion.start(Channel::first_pane, Event::directory_open, now));
    config.duration_scale = 4;
    config.durations_ms[0] = 2000;
    config.quality = core::MotionConfig::Quality::low;
    motion.configure(config);
    REQUIRE(motion.start(Channel::first_pane, Event::directory_open, now));
    CHECK(motion.frame(Channel::first_pane, now + 1s)->effect == Effect::fade);
    CHECK(motion.frame(Channel::first_pane, now + 1s)->progress == doctest::Approx(0.5));
    motion.advance(now + 2s);
    CHECK_FALSE(motion.active());
    CHECK(rummage::platform::appearance_from({}, {}, 1).reduced_motion);
    CHECK_FALSE(rummage::platform::appearance_from({}, {}, 2).reduced_motion);
    CHECK_FALSE(rummage::platform::appearance_from({}, {}).reduced_motion);
}

TEST_CASE("motion: config validates event names, types, finite durations and fallback") {
    const auto parsed = core::parse_config(R"(
[animations]
enabled = false
duration_scale = 1.5
reduce_motion = "always"
quality = "low"
[animations.events]
directory_open = "fade"
file_copy = "none"
[animations.durations]
directory_open = 550
)",
                                           "motion.toml");
    REQUIRE(parsed.problems.empty());
    CHECK_FALSE(parsed.config.animations.enabled);
    CHECK(parsed.config.animations.duration_scale == 1.5F);
    CHECK(parsed.config.animations.events[0] == Effect::fade);
    CHECK(parsed.config.animations.events[3] == Effect::none);
    CHECK(parsed.config.animations.durations_ms[0] == 550);
    for (const auto* value : {"nan", "inf", "-1", "5", "true", "'1'"}) {
        const auto bad =
            core::parse_config(std::string{"[animations]\nduration_scale="} + value, "test");
        CHECK(bad.problems.size() == 1);
        CHECK(bad.config.animations.duration_scale == 1);
    }
    const auto bad = core::parse_config(R"(
[animations]
enabled = 1
quality = "ultra"
reduce_motion = true
[animations.events]
made_up = "fade"
file_copy = "user.glsl"
[animations.durations]
directory_open = 2001
view_change = 2.5
)",
                                        "bad");
    CHECK(bad.problems.size() == 7);
    CHECK(bad.config.animations == core::MotionConfig{});
}

namespace {
std::array<unsigned char, 4> pixel(int x, int y) {
    std::array<unsigned char, 4> rgba{};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return rgba;
}

void save_frame(const std::filesystem::path& path, int width, int height) {
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width * height * 4));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgb.data());
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            out.write(reinterpret_cast<const char*>(rgb.data() +
                                                    static_cast<std::size_t>((y * width + x) * 4)),
                      3);
}
}  // namespace

TEST_CASE("motion: every GLES effect renders, preserves alpha and restores renderer state") {
    ui::Offscreen gl{320, 240};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    CHECK(renderer.motion_stats().texture_bytes == 0);
    for (const auto& effect : core::kMotionEffectRegistry) {
        if (effect.id == Effect::none)
            continue;
        CAPTURE(effect.name);
        for (const float progress : {0.0F, 0.45F, 1.0F}) {
            renderer.begin_frame(320, 240, 1, ui::Color::rgba(0x20406080));
            renderer.push_clip({20, 20, 280, 200});
            REQUIRE(
                renderer.begin_motion({20, 20, 280, 200}, core::MotionConfig::Quality::automatic));
            renderer.fill_rect({20, 20, 280, 200}, ui::Color::rgb(0x806040));
            renderer.fill_rect({40, 40, 40, 40}, ui::Color::rgb(0xE0A020));
            renderer.draw_text("Rummage motion", 50, 110, 18, ui::Color::rgb(0xFFFFFF));
            renderer.end_motion(
                {.effect = effect.id, .progress = progress, .time_seconds = progress * 0.3F}, 100,
                100, ui::Color::rgb(0x40C0FF));
            renderer.pop_clip();
            renderer.fill_rect({0, 0, 10, 10}, ui::Color::rgb(0x00FF00));
            renderer.end_frame();
            CHECK(glGetError() == GL_NO_ERROR);
            CHECK(pixel(5, 235)[1] == 255);    // drawing after the pass still works
            CHECK(pixel(310, 120)[3] == 128);  // outside clip untouched
            if (progress == 1) {
                CHECK(pixel(150, 190)[3] == (effect.id == Effect::disintegrate ? 128 : 255));
                CHECK(pixel(150, 190)[0] == (effect.id == Effect::disintegrate ? 16 : 128));
            }
            if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES");
                directory && progress == 0.45F)
                save_frame(std::filesystem::path{directory} / (std::string{effect.name} + ".ppm"),
                           320, 240);
        }
    }
    CHECK(renderer.motion_stats().allocations == 1);
    CHECK(renderer.motion_stats().texture_bytes == 280 * 200 * 8);
    renderer.begin_frame(320, 240, 1, ui::Color::rgb(0));
    REQUIRE(renderer.begin_motion({0, 0, 32, 32}, core::MotionConfig::Quality::automatic));
    renderer.fill_rect({0, 0, 32, 32}, ui::Color::rgb(0xFF0000));
    renderer.end_motion({.effect = Effect::fade, .progress = 1}, 16, 16, ui::Color::rgb(0));
    renderer.end_frame();
    CHECK(pixel(16, 224)[0] == 255);  // smaller captures sample only their valid subregion
    CHECK(renderer.motion_stats().allocations == 1);
    renderer.release_motion_textures();
    CHECK(renderer.motion_stats().texture_bytes == 0);
    renderer.begin_frame(320, 240, 1, ui::Color::rgb(0));
    REQUIRE(renderer.begin_motion({0, 0, 32, 32}, core::MotionConfig::Quality::automatic));
    // A release while capturing must leave the current pass intact.
    renderer.release_motion_textures();
    CHECK(renderer.motion_stats().texture_bytes == 32 * 32 * 8);
    renderer.fill_rect({0, 0, 32, 32}, ui::Color::rgb(0x00FF00));
    renderer.end_motion({.effect = Effect::fade, .progress = 1}, 16, 16, ui::Color::rgb(0));
    renderer.end_frame();
    CHECK(pixel(16, 224)[1] == 255);
    CHECK(renderer.motion_stats().allocations == 2);
    CHECK(glGetError() == GL_NO_ERROR);
    renderer.release_motion();
    CHECK(renderer.motion_stats().texture_bytes == 0);
}

TEST_CASE("motion: capture caps and fractional scales preserve normal rendering") {
    ui::Offscreen gl{2048, 2048};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    renderer.begin_frame(2048, 2048, 1, ui::Color::rgb(0x204060));
    CHECK_FALSE(renderer.begin_motion({0, 0, 2048, 2048}, core::MotionConfig::Quality::automatic));
    CHECK(renderer.motion_stats().texture_bytes == 0);
    REQUIRE(renderer.begin_motion({0, 0, 2048, 2048}, core::MotionConfig::Quality::high));
    renderer.fill_rect({0, 0, 2048, 2048}, ui::Color::rgb(0xFF0000));
    renderer.end_motion({.effect = Effect::fade, .progress = 1}, 0, 0, {});
    renderer.end_frame();
    CHECK(renderer.motion_stats().texture_bytes == 32 * 1024 * 1024);
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        renderer.begin_frame(2048, 2048, scale, ui::Color::rgba(0x20406080));
        renderer.push_clip({10, 10, 40, 40});
        REQUIRE(renderer.begin_motion({0, 0, 100, 100}, core::MotionConfig::Quality::automatic));
        renderer.fill_rect({0, 0, 100, 100}, ui::Color::rgb(0x00FF00));
        renderer.end_motion({.effect = Effect::fade, .progress = 1}, 0, 0, {});
        renderer.pop_clip();
        renderer.end_frame();
        const auto inside =
            pixel(static_cast<int>(20 * scale), 2048 - static_cast<int>(20 * scale));
        CHECK(inside[1] == 255);
        CHECK(inside[3] == 255);
        CHECK(pixel(1, 2047)[3] == 128);
        CHECK(renderer.motion_stats().texture_bytes <= 16 * 1024 * 1024);
        CHECK(glGetError() == GL_NO_ERROR);
    }
}

TEST_CASE("motion: browser navigation settles, reduced mode cancels and idle stays idle") {
    rummage::test::TempDir dir;
    std::filesystem::create_directory(dir.path / "child");
    std::ofstream{dir.path / "file.txt"} << "hello";
    core::EventLoop loop;
    core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::views::Browser browser{
        loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {}, [](ui::CursorShape) {}};
    ui::Offscreen gl{900, 600};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto colors = rummage::theme::Palette{};
    auto now = ui::Motion::Clock::now();
    const auto draw = [&] {
        renderer.begin_frame(900, 600, 1, colors.surface);
        browser.draw(renderer, {0, 0, 900, 600}, colors, icons, now);
        renderer.end_frame();
    };
    const auto settle = [&] {
        bool ready = false;
        const auto timer = loop.add_timer(1ms, 1ms, [&] {
            if (!browser.loading()) {
                ready = true;
                loop.quit(0);
            }
        });
        const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(timer);
        loop.remove(timeout);
        REQUIRE(ready);
    };
    browser.open(dir.path);
    settle();
    draw();
    CHECK_FALSE(browser.motion().active());
    browser.open(dir.path / "child");
    settle();
    draw();
    REQUIRE(browser.motion().frame(Channel::first_pane, now));
    CHECK(browser.motion().frame(Channel::first_pane, now)->effect == Effect::iris);
    now += 3s;
    draw();
    CHECK_FALSE(browser.motion().active());
    CHECK(renderer.motion_stats().texture_bytes == 0);
    const auto passes = renderer.motion_stats().passes;
    draw();
    CHECK(renderer.motion_stats().passes == passes);
    browser.back();
    settle();
    draw();
    REQUIRE(browser.motion().frame(Channel::first_pane, now));
    CHECK(browser.motion().frame(Channel::first_pane, now)->effect == Effect::ripple);
    CHECK(renderer.motion_stats().texture_bytes > 0);
    browser.set_reduced_motion(true);
    CHECK_FALSE(browser.motion().active());
    draw();
    CHECK(renderer.motion_stats().texture_bytes == 0);
    CHECK(glGetError() == GL_NO_ERROR);

    // Actual operations: the cue begins after the completion callback, never on submission.
    browser.set_reduced_motion(false);
    browser.set_home_trash(dir.path / "Trash");
    browser.picker_folder()->select_name("file.txt");
    now = ui::Motion::Clock::now();
    draw();
    browser.take_selection(false);
    browser.paste();
    CHECK_FALSE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    settle();
    REQUIRE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    CHECK(browser.motion().frame(Channel::operation, ui::Motion::Clock::now())->effect ==
          Effect::hologram);
    CHECK(std::filesystem::exists(dir.path / "file 2.txt"));
    now = ui::Motion::Clock::now() + 3s;
    draw();

    browser.toggle_split();
    browser.open(dir.path / "child");
    settle();
    browser.focus_pane(0);
    browser.picker_folder()->select_name("file.txt");
    draw();
    browser.transfer_to_other_pane(false);
    CHECK_FALSE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    settle();
    REQUIRE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    CHECK(browser.motion().frame(Channel::operation, ui::Motion::Clock::now())->effect ==
          Effect::hologram);
    CHECK(std::filesystem::exists(dir.path / "child" / "file.txt"));
    now = ui::Motion::Clock::now() + 3s;
    draw();

    // Both skip and cancellation leave the source intact, without a success cue.
    for (const auto key : {XKB_KEY_s, XKB_KEY_Escape}) {
        browser.transfer_to_other_pane(true);
        bool conflict = false;
        const auto timer = loop.add_timer(1ms, 1ms, [&] {
            if (browser.conflict_dialog().is_open()) {
                conflict = true;
                loop.quit(0);
            }
        });
        const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(timer);
        loop.remove(timeout);
        REQUIRE(conflict);
        browser.conflict_dialog().key({.keysym = static_cast<xkb_keysym_t>(key), .text = {}});
        settle();
        CHECK_FALSE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
        CHECK(std::filesystem::exists(dir.path / "file.txt"));
    }
    browser.picker_folder()->select_name("file 2.txt");
    draw();
    browser.transfer_to_other_pane(true);
    settle();
    REQUIRE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    CHECK(browser.motion().frame(Channel::operation, ui::Motion::Clock::now())->effect ==
          Effect::portal);
    CHECK_FALSE(std::filesystem::exists(dir.path / "file 2.txt"));
    CHECK(std::filesystem::exists(dir.path / "child" / "file 2.txt"));
    now = ui::Motion::Clock::now() + 3s;
    draw();
    browser.picker_folder()->select_name("file.txt");
    draw();
    browser.trash();
    CHECK_FALSE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    settle();
    REQUIRE(browser.motion().frame(Channel::operation, ui::Motion::Clock::now()));
    CHECK(browser.motion().frame(Channel::operation, ui::Motion::Clock::now())->effect ==
          Effect::disintegrate);
    CHECK_FALSE(std::filesystem::exists(dir.path / "file.txt"));

    if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES")) {
        browser.set_mode(rummage::views::Browser::Mode::grid);
        now = ui::Motion::Clock::now();
        draw();
        now += 100ms;
        draw();
        save_frame(std::filesystem::path{directory} / "browser.ppm", 900, 600);
    }
}
