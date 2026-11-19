#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/kde.hpp"
#include "rummage/theme/theme.hpp"
#include "temp_dir.hpp"

using rummage::test::TempDir;
using namespace rummage::theme;

namespace {

// Representative active Breeze colours. No KDE runtime or installed scheme is needed.
std::string scheme(bool dark = false) {
    const std::string text = dark ? "252,252,252" : "35,38,41";
    const std::string view = dark ? "20,22,24" : "255,255,255";
    const std::string window = dark ? "32,35,38" : "239,240,241";
    return "[General]\nColorScheme=Breeze\n[Colors:View]\nBackgroundNormal=" + view +
           "\nForegroundNormal=" + text + "\nBackgroundAlternate=" + window +
           "\nForegroundInactive=127,140,141\nForegroundLink=41,128,185\n"
           "ForegroundNegative=218,68,83\n[Colors:Window]\nBackgroundNormal=" +
           window + "\nForegroundNormal=" + text + "\n[Colors:Button]\nBackgroundNormal=" + window +
           "\nForegroundNormal=" + text +
           "\n[Colors:Selection]\nBackgroundNormal=61,174,233\nForegroundNormal=255,255,255\n";
}

bool same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

PaletteResult parse(std::string_view text) { return parse_kde_palette(text, "kdeglobals"); }

void settle(rummage::core::EventLoop& loop) {
    using namespace std::chrono_literals;
    loop.add_timer(250ms, 0ms, [&] { loop.quit(0); });
    loop.run();
}

}  // namespace

TEST_CASE("KDE: active light and dark colours retain surfaces with readable text") {
    for (const bool dark : {false, true}) {
        const auto result = parse(scheme(dark));
        REQUIRE(result.problems.empty());
        REQUIRE(result.from_file);
        const auto& p = result.palette;
        CHECK(p.dark == dark);
        CHECK(same(p.surface, Color::rgb(dark ? 0x141618 : 0xFFFFFF)));
        CHECK(same(p.surface_container, Color::rgb(dark ? 0x202326 : 0xEFF0F1)));
        CHECK(same(p.on_surface, Color::rgb(dark ? 0xFCFCFC : 0x232629)));
        CHECK(same(p.primary, Color::rgb(0x3DAEE9)));
        CHECK(same(p.hover, p.primary));
        for (const Color bg :
             {p.surface, p.surface_variant, p.surface_container_lowest, p.surface_container_low,
              p.surface_container, p.surface_container_high, p.surface_container_highest,
              p.surface_dim, p.surface_bright}) {
            CHECK(contrast(p.on_surface, bg) >= 4.5F);
            CHECK(contrast(p.on_surface_variant, bg) >= 4.5F);
            CHECK(contrast(p.secondary, bg) >= 4.5F);
            CHECK(contrast(p.error, bg) >= 4.5F);
        }
        CHECK(contrast(p.on_primary, p.primary) >= 4.5F);
        CHECK(contrast(p.on_hover, p.hover) >= 4.5F);
        CHECK_FALSE(same(p.on_primary, Color::rgb(0xFFFFFF)));  // unreadable raw Breeze pair
    }
}

TEST_CASE("KDE: active groups ignore unrelated settings and inactive effects") {
    auto input = scheme();
    input +=
        "[Colors:View][Inactive]\nBackgroundNormal=bad\nForegroundNormal=bad\n"
        "[ColorEffects:Inactive]\nIntensityAmount=invalid\n[Icons]\nTheme=breeze\n";
    const auto result = parse(input);
    REQUIRE(result.from_file);
    CHECK(same(result.palette.surface, Color::rgb(0xFFFFFF)));
    input = scheme();
    input.replace(input.find("[Colors:View]"), 13, "[Colors:View][$i]");
    input.replace(input.find("BackgroundNormal=255,255,255"), 28,
                  "BackgroundNormal[$i]= 255, 255, 255, 255\n");
    CHECK(parse(input).from_file);
}

TEST_CASE("KDE: malformed or incomplete colour input is rejected atomically") {
    for (const auto bad :
         {"", "255,0", "256,0,0", "-1,0,0", "0.5,0,0", "#ffffff", "255,255,255,0",
          "255,255,255,254", "0,0,0,255,0", "0,0,0,", "0,0,0oops", "999999999999999999999,0,0"}) {
        auto input = scheme();
        input.replace(input.find("61,174,233"), 10, bad);
        const auto result = parse(input);
        CHECK_FALSE(result.from_file);
        REQUIRE_FALSE(result.problems.empty());
        CHECK(result.problems.front().starts_with("kdeglobals:"));
    }
    for (const std::string& extra : std::array<std::string, 6>{
             "[Colors:View]\nBackgroundNormal=0,0,0\n", "[Colors:View][$d]\n",
             "[Colors:View]\nForegroundNormal[$e]=1,2,3\n", "[Colors:View]\nbroken\n",
             "[Colors:View\n", std::string(1, '\0')}) {
        CHECK_FALSE(parse(scheme() + extra).from_file);
    }
    CHECK_FALSE(parse("[General]\nColorScheme=Breeze\n").from_file);
    CHECK_FALSE(parse(std::string(1024 * 1024 + 1, '#')).from_file);
    auto incompatible = scheme();
    const auto button =
        incompatible.find("BackgroundNormal=239,240,241", incompatible.find("[Colors:Button]"));
    incompatible.replace(button, 28, "BackgroundNormal=0,0,0");
    const auto rejected = parse(incompatible);
    CHECK_FALSE(rejected.from_file);
    REQUIRE_FALSE(rejected.problems.empty());
    CHECK(rejected.problems.front().find("incompatible text") != std::string::npos);
}

TEST_CASE("KDE: parser diagnostics and file IO are bounded") {
    const TempDir dir;
    CHECK(load_kde_palette(dir.path / "missing").problems.empty());
    CHECK_FALSE(load_kde_palette(dir.path).problems.empty());
    const auto fifo = dir.path / "fifo";
    REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);
    CHECK_FALSE(load_kde_palette(fifo).problems.empty());  // must not block opening a pipe
    const auto large = dir.write("large", "");
    std::filesystem::resize_file(large, 1024 * 1024 + 1);
    CHECK_FALSE(load_kde_palette(large).problems.empty());
    std::string malformed = "[Colors:View]\n";
    for (int i = 0; i < 10000; ++i)
        malformed += "bad\n";
    CHECK(parse(malformed).problems.size() <= 32);
    const auto file = dir.write("kdeglobals", scheme());
    const auto link = dir.path / "link";
    std::filesystem::create_symlink(file, link);
    CHECK(load_kde_palette(link).from_file);
    CHECK(default_kde_colors_path() ==
          rummage::core::default_config_path().parent_path().parent_path() / "kdeglobals");
}

TEST_CASE("KDE: explicit source keeps automatic precedence and mode override independent") {
    const TempDir dir;
    const auto kde = dir.write("kdeglobals", scheme());
    const auto colors = dir.write("colors.toml", "[colors]\nprimary='#ff0000'\n");
    Theme theme{colors, kde};
    theme.set_desktop(Desktop{.dark = true, .accent = Color::rgb(0x00FF00), .high_contrast = true});
    CHECK(theme.active_source() == Theme::Source::file);
    theme.set_source(Theme::Source::kde);
    CHECK(theme.active_source() == Theme::Source::kde);
    CHECK_FALSE(theme.palette().dark);
    REQUIRE(theme.accent());
    CHECK(same(*theme.accent(), Color::rgb(0x3DAEE9)));
    theme.set_dark_override(true);
    CHECK(theme.palette().dark);
    CHECK(contrast(theme.palette().on_surface, theme.palette().surface) >= 7.0F);
    theme.set_dark_override(std::nullopt);
    CHECK(same(theme.palette().surface, Color::rgb(0xFFFFFF)));
    dir.write("kdeglobals", "invalid");
    theme.reload_kde();
    CHECK(theme.status().retaining_last_valid_kde);
    CHECK(same(theme.palette().surface, Color::rgb(0xFFFFFF)));
    REQUIRE_FALSE(theme.status().problems().empty());
    std::filesystem::remove(kde);
    theme.reload_kde();
    CHECK(theme.status().kde_state == Theme::FileState::missing);
    CHECK(theme.status().requested == Theme::Source::kde);
    CHECK(theme.active_source() == Theme::Source::builtin);
    CHECK_FALSE(theme.accent());
    theme.set_source(Theme::Source::automatic);
    CHECK(theme.active_source() == Theme::Source::file);
    std::filesystem::remove(colors);
    theme.reload();
    CHECK(theme.active_source() == Theme::Source::desktop);
}

TEST_CASE("KDE: watcher follows creation, replacement, invalid edits, removal and recreation") {
    const TempDir dir;
    rummage::core::EventLoop loop;
    Theme theme{{}, dir.path / "kdeglobals"};
    theme.watch(loop);
    theme.set_source(Theme::Source::kde);
    CHECK(theme.active_source() == Theme::Source::builtin);
    dir.write("kdeglobals", scheme());
    settle(loop);
    REQUIRE(theme.active_source() == Theme::Source::kde);
    CHECK_FALSE(theme.palette().dark);
    REQUIRE_FALSE(rummage::core::write_file_atomically(dir.path / "kdeglobals", scheme(true)));
    settle(loop);
    CHECK(theme.palette().dark);
    dir.write("kdeglobals", "[Colors:View]\nBackgroundNormal=broken\n");
    settle(loop);
    CHECK(theme.status().retaining_last_valid_kde);
    CHECK(theme.palette().dark);
    std::filesystem::remove(dir.path / "kdeglobals");
    settle(loop);
    CHECK(theme.active_source() == Theme::Source::builtin);
    theme.set_source(Theme::Source::builtin);
    dir.write("kdeglobals", scheme());
    settle(loop);
    CHECK(theme.palette().dark);
    theme.set_source(Theme::Source::kde);
    CHECK_FALSE(theme.palette().dark);
}
