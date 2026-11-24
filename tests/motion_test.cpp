#include "rummage/ui/motion.hpp"

#include <GLES3/gl3.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/platform/portal.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/settings_dialog.hpp"
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
    CHECK_FALSE(browser.ambient_interval());
    core::MotionConfig ambient_config;
    ambient_config.ambient.background = true;
    ambient_config.ambient.selection_glow = true;
    ambient_config.ambient.fps = 24;
    browser.set_motion_config(ambient_config);
    draw();
    REQUIRE(browser.ambient_interval());
    CHECK(*browser.ambient_interval() == 42ms);
    CHECK_FALSE(browser.animating());
    CHECK(renderer.motion_stats().texture_bytes == 0);
    browser.set_effects_active(false);
    CHECK_FALSE(browser.ambient_interval());
    browser.set_effects_active(true);
    browser.set_reduced_motion(true);
    draw();
    CHECK_FALSE(browser.ambient_interval());
    browser.set_reduced_motion(false);
    ambient_config.ambient.speed = 0;
    browser.set_motion_config(ambient_config);
    draw();
    CHECK_FALSE(browser.ambient_interval());
    browser.set_motion_config({});
    draw();
    CHECK_FALSE(browser.ambient_interval());
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

TEST_CASE("motion: ambient settings validate bounds and share accessibility policy") {
    const auto parsed = core::parse_config(R"toml(
[animations]
hologram_spread = 8
portal_rotation = 6
[animations.ambient]
background = true
selection_glow = true
speed = 0.5
intensity = 0.4
fps = 24
shader = "/tmp/background.frag"
)toml",
                                           "test");
    REQUIRE(parsed.problems.empty());
    auto config = parsed.config.animations;
    ui::Motion motion;
    motion.configure(config);
    CHECK(motion.ambient_enabled());
    REQUIRE(motion.start(Channel::operation, Event::file_copy, {}));
    CHECK(motion.frame(Channel::operation, {})->hologram_spread == 8);
    CHECK(motion.frame(Channel::operation, {})->portal_rotation == 6);
    motion.set_system_reduced(true);
    CHECK_FALSE(motion.ambient_enabled());
    CHECK_FALSE(motion.active());
    config.reduce_motion = core::MotionConfig::Reduce::never;
    motion.configure(config);
    CHECK(motion.ambient_enabled());
    config.ambient.intensity = 0;
    motion.configure(config);
    CHECK_FALSE(motion.ambient_enabled());
    config.ambient.intensity = 1;
    config.enabled = false;
    motion.configure(config);
    CHECK_FALSE(motion.ambient_enabled());
    for (const auto invalid :
         {"[animations]\nhologram_spread = nan", "[animations]\nportal_rotation = 13",
          "[animations.ambient]\nspeed = inf", "[animations.ambient]\nintensity = -1",
          "[animations.ambient]\nfps = 0", "[animations.ambient]\nfps = 61",
          "[animations.ambient]\nbackground = 1", "[animations.ambient]\nshader = 'relative.frag'",
          "[animations.ambient]\nunknown = true"}) {
        CAPTURE(invalid);
        CHECK_FALSE(core::parse_config(invalid, "test").problems.empty());
    }
}

TEST_CASE("motion: persistent shaders animate without captures and preserve background alpha") {
    ui::Offscreen gl{320, 240};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    core::MotionConfig::Ambient config;
    config.intensity = 1;
    std::array<unsigned char, 4> first{};
    for (const bool selection : {false, true}) {
        for (const float seconds : {0.0F, 2.0F}) {
            renderer.begin_frame(320, 240, 1, ui::Color::rgba(0x20406080));
            renderer.push_clip({20, 20, 160, 120});
            REQUIRE(renderer.draw_ambient(
                selection ? ui::Rect{40, 40, 100, 60} : ui::Rect{10, 10, 200, 180}, config, seconds,
                ui::Color::rgb(0xFF8000), selection));
            renderer.pop_clip();
            renderer.fill_rect({0, 0, 10, 10}, ui::Color::rgb(0x00FF00));
            renderer.end_frame();
            CHECK(glGetError() == GL_NO_ERROR);
            CHECK(pixel(5, 235)[1] == 255);
            CHECK(pixel(310, 120)[3] == 128);
            if (!selection) {
                const auto sample = pixel(70, 170);
                CHECK(sample[3] == 128);
                CHECK(sample[0] <= sample[3]);
                if (seconds == 0)
                    first = sample;
                else
                    CHECK(sample != first);
            }
            if (selection) {
                CHECK(pixel(40, 190)[3] > 128);
                CHECK(pixel(90, 170)[3] == 128);
            }
            CHECK(renderer.motion_stats().texture_bytes == 0);
            CHECK(renderer.motion_stats().allocations == 0);
            if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
                save_frame(std::filesystem::path{directory} /
                               (selection ? "ambient-selection.ppm" : "ambient-background.ppm"),
                           320, 240);
        }
    }
}

TEST_CASE("motion: custom shader reload keeps last good pixels after errors and recovers") {
    rummage::test::TempDir dir;
    const auto path = dir.path / "background.frag";
    ui::Offscreen gl{80, 60};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    core::MotionConfig::Ambient config;
    config.shader = path.string();
    config.intensity = 1;
    const auto draw = [&] {
        renderer.begin_frame(80, 60, 1, ui::Color::rgb(0x000000));
        REQUIRE(renderer.draw_ambient({0, 0, 80, 60}, config, 0, ui::Color::rgb(0xFFFFFF)));
        renderer.end_frame();
        CHECK(glGetError() == GL_NO_ERROR);
        return pixel(30, 30);
    };
    std::ofstream{path} << "vec4 effect(vec2 p) { return vec4(1, 0, 0, 1); }";
    CHECK(draw()[0] == 255);
    CHECK(renderer.ambient_error().empty());
    std::ofstream{path} << "invalid GLSL";
    std::this_thread::sleep_for(510ms);
    CHECK(draw()[0] == 255);
    CHECK_FALSE(renderer.ambient_error().empty());
    std::ofstream{path} << "vec4 effect(vec2 p) { return vec4(0, 1, 0, 1); }";
    std::this_thread::sleep_for(510ms);
    CHECK(draw()[1] == 255);
    CHECK(renderer.ambient_error().empty());
    config.shader = (dir.path / "missing.frag").string();
    CHECK(draw()[1] == 255);
    CHECK_FALSE(renderer.ambient_error().empty());
    config.shader = dir.path.string();
    CHECK(draw()[1] == 255);
    CHECK_FALSE(renderer.ambient_error().empty());
    const auto huge = dir.path / "huge.frag";
    std::ofstream{huge} << std::string(65537, 'x');
    config.shader = huge.string();
    CHECK(draw()[1] == 255);
    CHECK_FALSE(renderer.ambient_error().empty());
    config.shader.clear();
    draw();
    CHECK(renderer.ambient_error().empty());
}

TEST_CASE("motion: effects preview draws at wide and narrow sizes without retaining captures") {
    rummage::test::TempDir dir;
    const auto path = dir.path / "config.toml";
    std::ofstream{path} << "[animations.ambient]\nbackground=true\nselection_glow=true\n";
    core::EventLoop loop;
    core::WorkerPool pool{loop, 1};
    rummage::views::SettingsDialog settings{pool, [] {}};
    settings.open(path, [] {}, [&](const core::ConfigResult&) {});
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (!settings.busy())
            loop.quit(0);
    });
    const auto timeout = loop.add_timer(3s, 0ms, [&] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(timeout);
    REQUIRE_FALSE(settings.busy());
    REQUIRE(settings.error().empty());
    ui::Offscreen gl{1000, 800};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto palette = rummage::theme::Palette{};
    settings.preview_effect(Event::file_copy);
    settings.set_system_reduced(true);
    for (const auto width : {1000, 480}) {
        renderer.begin_frame(width, 800, 1, palette.surface);
        settings.draw(renderer, {0, 0, static_cast<float>(width), 800}, palette, icons);
        renderer.end_frame();
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK_FALSE(settings.animating());
        CHECK(renderer.motion_stats().texture_bytes == 0);
        if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
            save_frame(std::filesystem::path{directory} /
                           ("effects-preview-" + std::to_string(width) + ".ppm"),
                       width, 800);
    }
    settings.set_system_reduced(false);
    renderer.begin_frame(1000, 800, 1, palette.surface);
    settings.draw(renderer, {0, 0, 1000, 800}, palette, icons);
    renderer.end_frame();
    if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
        save_frame(std::filesystem::path{directory} / "effects-preview-enabled.ppm", 1000, 800);
    CHECK(renderer.motion_stats().texture_bytes == 0);
    for (std::size_t slot = 0; slot < core::kMotionEvents.size() + 2; ++slot) {
        settings.select_effect_slot(slot);
        renderer.begin_frame(1000, 800, 1, palette.surface);
        settings.draw(renderer, {0, 0, 1000, 800}, palette, icons);
        renderer.end_frame();
        CHECK(glGetError() == GL_NO_ERROR);
        if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
            save_frame(std::filesystem::path{directory} /
                           ("effects-slot-" + std::to_string(slot) + ".ppm"),
                       1000, 800);
    }
    settings.close();
    CHECK_FALSE(settings.ambient_interval());
}

TEST_CASE("motion: object shader parameters change intermediate pixels but preserve endpoints") {
    ui::Offscreen gl{120, 100};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    const auto draw = [&](Effect effect, float parameter, float progress) {
        renderer.begin_frame(120, 100, 1, ui::Color::rgb(0x102030));
        REQUIRE(
            renderer.begin_motion({10, 10, 100, 80}, core::MotionConfig::Quality::automatic, true));
        renderer.fill_rect({30, 30, 30, 25}, ui::Color::rgb(0xFF0000));
        renderer.fill_rect({60, 30, 20, 25}, ui::Color::rgb(0x0080FF));
        renderer.end_motion({.effect = effect,
                             .progress = progress,
                             .hologram_spread = parameter,
                             .portal_rotation = parameter},
                            60, 50, ui::Color::rgb(0xFFFFFF));
        renderer.end_frame();
        std::vector<unsigned char> pixels(120 * 100 * 4);
        glReadPixels(0, 0, 120, 100, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    for (const auto effect : {Effect::hologram, Effect::portal}) {
        CAPTURE(static_cast<int>(effect));
        CHECK(draw(effect, 0, 0.3F) != draw(effect, 6, 0.3F));
        CHECK(draw(effect, 0, 1) == draw(effect, 6, 1));
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("motion: shader drafts keep separate programs and diagnostics from the browser") {
    rummage::test::TempDir dir;
    const auto live = dir.path / "live.frag";
    const auto draft = dir.path / "draft.frag";
    std::ofstream{live} << "vec4 effect(vec2 p) { return vec4(1, 0, 0, 1); }";
    std::ofstream{draft} << "vec4 effect(vec2 p) { return vec4(0, 1, 0, 1); }";
    ui::Offscreen gl{120, 60};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    core::MotionConfig::Ambient browser, preview;
    browser.shader = live.string();
    preview.shader = draft.string();
    browser.intensity = preview.intensity = 1;
    const auto draw = [&] {
        renderer.begin_frame(120, 60, 1, ui::Color::rgb(0));
        REQUIRE(renderer.draw_ambient({0, 0, 60, 60}, browser, 0, {}, false, false));
        REQUIRE(renderer.draw_ambient({60, 0, 60, 60}, preview, 0, {}, false, true));
        renderer.end_frame();
        CHECK(pixel(30, 30)[0] == 255);
        CHECK(pixel(90, 30)[1] == 255);
    };
    draw();
    preview.shader = (dir.path / "missing.frag").string();
    draw();
    CHECK(renderer.ambient_error().empty());
    CHECK_FALSE(renderer.ambient_error(true).empty());
    renderer.release_ambient(true);
    CHECK(renderer.ambient_error().empty());
    CHECK(renderer.ambient_error(true).empty());
}

TEST_CASE("motion: move and trash retain the painted thumbnail after the source disappears") {
    using Browser = rummage::views::Browser;
    for (const auto mode : {Browser::Mode::grid, Browser::Mode::details, Browser::Mode::columns}) {
        for (const bool move : {false, true}) {
            CAPTURE(static_cast<int>(mode));
            CAPTURE(move);
            rummage::test::TempDir dir;
            const auto source = dir.path / "source";
            const auto destination = dir.path / "destination";
            std::filesystem::create_directory(source);
            std::filesystem::create_directory(destination);
            rummage::picture::Image image{
                .width = 100, .height = 50, .rgba = std::vector<std::uint8_t>(100 * 50 * 4)};
            for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
                image.rgba[i] = image.rgba[i + 2] = image.rgba[i + 3] = 255;
            }
            const auto png = rummage::picture::encode_png(image);
            const auto file = source / "photo.png";
            {
                std::ofstream out{file, std::ios::binary};
                out.write(reinterpret_cast<const char*>(png.data()),
                          static_cast<std::streamsize>(png.size()));
            }
            // The GL context outlives the browser and its pinned thumbnail cache.
            ui::Offscreen gl{1000, 700};
            ui::FontSystem fonts;
            ui::Renderer renderer{fonts};
            ui::IconSet icons{fonts};
            core::EventLoop loop;
            core::WorkerPool pool{loop, 2};
            rummage::fs::DirectoryWatcher watcher{loop};
            const auto cache = std::make_shared<rummage::thumbs::Cache>(dir.path / "cache", "test");
            Browser browser{loop,    pool,
                            watcher, rummage::fs::MimeDatabase::load(),
                            [] {},   [](ui::CursorShape) {},
                            cache};
            browser.set_info_visible(false);
            browser.set_home_trash(dir.path / "Trash");
            core::MotionConfig config;
            config.events.fill(Effect::none);
            for (const auto event : {Event::file_move, Event::file_trash}) {
                config.events[static_cast<std::size_t>(event)] = Effect::fade;
                config.durations_ms[static_cast<std::size_t>(event)] = 1000;
            }
            browser.set_motion_config(config);
            const auto colors = rummage::theme::Palette{};
            auto now = ui::Motion::Clock::now();
            const auto draw = [&] {
                renderer.begin_frame(1000, 700, 1, colors.surface);
                browser.draw(renderer, {0, 0, 1000, 700}, colors, icons, now);
                renderer.end_frame();
            };
            const auto settle = [&] {
                bool ready = false;
                const auto timer = loop.add_timer(1ms, 1ms, [&] {
                    if (!browser.loading()) {
                        ready = true;
                        loop.quit();
                    }
                });
                const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(); });
                loop.run();
                loop.remove(timer);
                loop.remove(timeout);
                REQUIRE(ready);
            };
            browser.open(source);
            settle();
            if (move) {
                browser.toggle_split();
                browser.open(destination);
                settle();
                browser.focus_pane(0);
            }
            browser.set_mode(mode);
            browser.set_grid_size(3);
            browser.picker_folder()->select_name("photo.png");
            std::vector<unsigned char> before(1000 * 700 * 4);
            // MIME sniffing can finish after the first layout. Pump draws until the view has
            // actually painted the thumbnail, rather than capturing its loading placeholder.
            bool painted = false;
            const auto poll = loop.add_timer(1ms, 1ms, [&] {
                draw();
                glReadPixels(0, 0, 1000, 700, GL_RGBA, GL_UNSIGNED_BYTE, before.data());
                for (std::size_t i = 0; i < before.size(); i += 4)
                    if (before[i] == 255 && before[i + 1] == 0 && before[i + 2] == 255) {
                        painted = true;
                        loop.quit();
                        break;
                    }
            });
            const auto deadline = loop.add_timer(5s, 0ms, [&] { loop.quit(); });
            loop.run();
            loop.remove(poll);
            loop.remove(deadline);
            REQUIRE(painted);
            if (move)
                browser.transfer_to_other_pane(true);
            else
                browser.trash();
            settle();
            REQUIRE_FALSE(std::filesystem::exists(file));
            now = ui::Motion::Clock::now() + 500ms;
            const auto frame = browser.motion().frame(Channel::operation, now);
            REQUIRE(frame);
            const auto start = frame->from;
            const auto original = static_cast<std::size_t>(
                                      (699 - static_cast<int>(start.y + start.height / 2)) * 1000 +
                                      static_cast<int>(start.x + start.width / 2)) *
                                  4;
            CHECK(before[original] == 255);
            CHECK(before[original + 1] == 0);
            CHECK(before[original + 2] == 255);
            if (mode == Browser::Mode::grid)
                CHECK(start.width >
                      32);  // Keep the displayed size instead of a generic small icon.
            draw();
            const auto rect = frame->geometry();
            const auto middle = pixel(static_cast<int>(rect.x + rect.width / 2),
                                      699 - static_cast<int>(rect.y + rect.height / 2));
            CHECK(middle[0] > 180);
            CHECK(middle[1] < 80);
            CHECK(middle[2] > 180);
            // The landscape thumbnail remains letterboxed, never stretched into a square.
            const auto above = pixel(static_cast<int>(rect.x + rect.width / 2),
                                     699 - static_cast<int>(rect.y + rect.height * 0.1F));
            CHECK_FALSE((above[0] > 180 && above[1] < 80 && above[2] > 180));
            CHECK(glGetError() == GL_NO_ERROR);
            if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
                save_frame(std::filesystem::path{directory} /
                               ("thumbnail-" + std::to_string(static_cast<int>(mode)) +
                                (move ? "-move.ppm" : "-trash.ppm")),
                           1000, 700);
            now += 3s;
            draw();
            CHECK_FALSE(browser.motion().active());
            CHECK(renderer.motion_stats().texture_bytes == 0);
        }
    }
}

TEST_CASE("motion: portal collapses at the source and emerges at the destination") {
    ui::Motion motion;
    const ui::Motion::Clock::time_point now{};
    REQUIRE(motion.start(Channel::operation, Event::file_move, now, {10, 20, 32, 32},
                         {200, 100, 24, 24}));
    CHECK(motion.frame(Channel::operation, now + 100ms)->geometry().x == 10);
    CHECK(motion.frame(Channel::operation, now + 209ms)->geometry().x == 10);
    CHECK(motion.frame(Channel::operation, now + 210ms)->geometry().x == 200);
    CHECK(motion.frame(Channel::operation, now + 320ms)->geometry().width == 24);
}

TEST_CASE("motion: isolated effects preserve backdrop, clips and premultiplied alpha") {
    ui::Offscreen gl{160, 120};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    for (const auto effect : {Effect::disintegrate, Effect::hologram, Effect::portal}) {
        CAPTURE(static_cast<int>(effect));
        for (const float progress : {0.0F, 0.3F, 0.5F, 0.75F, 1.0F}) {
            renderer.begin_frame(160, 120, 1, ui::Color::rgba(0x20406080));
            renderer.fill_rect({0, 0, 10, 120}, ui::Color::rgb(0xFF0000));
            renderer.push_clip({10, 10, 140, 100});
            REQUIRE(renderer.begin_motion({0, 0, 160, 120}, core::MotionConfig::Quality::automatic,
                                          true));
            renderer.fill_rect({64, 44, 32, 32}, ui::Color::rgb(0xC08020));
            renderer.end_motion({.effect = effect,
                                 .progress = progress,
                                 .from = {64, 44, 32, 32},
                                 .to = {64, 44, 32, 32}},
                                80, 60, ui::Color::rgb(0x40C0FF));
            renderer.pop_clip();
            renderer.fill_rect({150, 0, 10, 10}, ui::Color::rgb(0x00FF00));
            renderer.end_frame();
            CHECK(pixel(5, 60) == std::array<unsigned char, 4>{255, 0, 0, 255});
            CHECK(pixel(155, 115)[1] == 255);
            CHECK(pixel(12, 108) == std::array<unsigned char, 4>{16, 32, 48, 128});
            if (progress == 0 || progress == 1) {
                const bool erased = progress == 1 && effect == Effect::disintegrate;
                CHECK(pixel(80, 60)[3] == (erased ? 128 : 255));
                CHECK(pixel(80, 60)[0] == (erased ? 16 : 192));
            }
            if (effect == Effect::portal && progress == 0.5F)
                CHECK(pixel(80, 60) == std::array<unsigned char, 4>{16, 32, 48, 128});
            std::vector<unsigned char> pixels(160 * 120 * 4);
            glReadPixels(0, 0, 160, 120, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            bool valid = true;
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                valid = valid && pixels[i] <= pixels[i + 3] && pixels[i + 1] <= pixels[i + 3] &&
                        pixels[i + 2] <= pixels[i + 3];
            CHECK(valid);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("motion: render operation filmstrip with actual icon shaders") {
    ui::Offscreen gl{960, 540};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    constexpr std::array effects{Effect::disintegrate, Effect::hologram, Effect::portal};
    constexpr std::array names{"Delete", "Copy", "Move"};
    constexpr std::array phases{0.0F, 0.2F, 0.4F, 0.6F, 0.8F, 1.0F};
    renderer.begin_frame(960, 540, 1, ui::Color::rgb(0x0B1019));
    for (std::size_t row = 0; row < effects.size(); ++row) {
        for (std::size_t column = 0; column < phases.size(); ++column) {
            const float x = static_cast<float>(column) * 160;
            const float y = static_cast<float>(row) * 180;
            renderer.push_clip({x, y, 160, 180});
            renderer.draw_text(names[row], x + 12, y + 20, 13, ui::Color::rgb(0xB4C4D8));
            const ui::Rect rect{x + 48, y + 55, 64, 64};
            REQUIRE(renderer.begin_motion(rect.inset(-48), core::MotionConfig::Quality::automatic,
                                          true));
            icons.draw(renderer, "file", rect.x, rect.y, rect.width, ui::Color::rgb(0xC3DCF4));
            renderer.end_motion(
                {.effect = effects[row], .progress = phases[column], .from = rect, .to = rect},
                rect.x + 32, rect.y + 32, ui::Color::rgb(0x40C0FF));
            renderer.pop_clip();
        }
    }
    renderer.end_frame();
    CHECK(glGetError() == GL_NO_ERROR);
    if (const auto* directory = std::getenv("RUMMAGE_MOTION_FRAMES"))
        save_frame(std::filesystem::path{directory} / "operation-filmstrip.ppm", 960, 540);
}
