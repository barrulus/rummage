#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>

#include <doctest/doctest.h>

#include "rummage/text/text.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/syntax.hpp"

using rummage::theme::Color;
using rummage::theme::contrast;
using rummage::theme::desktop_palette;
using rummage::theme::Palette;
using rummage::theme::syntax_colors;
using rummage::theme::SyntaxColors;

namespace {

// As in desktop_palette_test.cpp: GNOME's accents, Plasma's, and the hard cases.
constexpr std::array kAccents{
    0x3584E4U, 0x2190A4U, 0x3A944AU, 0xC88800U, 0xED5B00U, 0xE62D42U, 0xD56199U, 0x9141ACU,
    0x6F8396U, 0x3DAEE9U, 0xFFFFFFU, 0x000000U, 0x808080U, 0xFFFF00U, 0x0000FFU,
};

// One step of an 8-bit channel.
constexpr float kClose = 1.0F / 255.0F;

bool same(Color a, Color b) {
    return std::abs(a.r - b.r) <= kClose && std::abs(a.g - b.g) <= kClose &&
           std::abs(a.b - b.b) <= kClose;
}

void every_colour_reads(const Palette& palette) {
    const SyntaxColors colours = syntax_colors(palette);
    for (std::size_t i = 0; i < colours.size(); ++i) {
        CAPTURE(i);
        CHECK(colours.at(i).a == 1.0F);  // opaque
        CHECK(contrast(colours.at(i), palette.surface_container) >= 4.5F);
    }
}

}  // namespace

TEST_CASE("syntax colours: one per style") {
    CHECK(rummage::theme::kSyntaxColors == rummage::text::kStyleCount);
}

TEST_CASE("syntax colours: Noctalia's palette as it is, every colour readable") {
    const Palette noctalia;
    every_colour_reads(noctalia);
    const SyntaxColors colours = syntax_colors(noctalia);
    // Its colours read already: kept as they are.
    CHECK(same(colours.at(0), noctalia.primary));    // keyword
    CHECK(same(colours.at(1), noctalia.tertiary));   // type
    CHECK(same(colours.at(2), noctalia.secondary));  // string
    CHECK(same(colours.at(7), noctalia.error));      // removed
    // Keywords, types, strings, comments and numbers apart.
    for (std::size_t a = 0; a < 5; ++a) {
        for (std::size_t b = a + 1; b < 5; ++b) {
            CAPTURE(std::format("{} {}", a, b));
            CHECK_FALSE(same(colours.at(a), colours.at(b)));
        }
    }
}

TEST_CASE("syntax colours: readable on every desktop palette, dark and light") {
    for (const bool dark : {true, false}) {
        for (const std::uint32_t accent : kAccents) {
            CAPTURE(std::format("#{:06x} {}", accent, dark ? "dark" : "light"));
            every_colour_reads(desktop_palette({.dark = dark, .accent = Color::rgb(accent)}));
        }
    }
}

TEST_CASE("syntax colours: a palette whose colours do not read on its page is mended") {
    Palette bad;
    bad.surface_container = Color::rgb(0x202020);
    bad.primary = Color::rgb(0x202020);  // the page's own colour
    bad.secondary = Color::rgb(0x252530);
    bad.tertiary = Color::rgb(0x1A1A1A);
    bad.error = Color::rgb(0x400000);
    bad.on_surface_variant = Color::rgb(0x303030);
    every_colour_reads(bad);
    Palette light = bad;
    light.surface_container = Color::rgb(0xF0F0F0);
    light.primary = Color::rgb(0xFFFF80);
    light.secondary = Color::rgb(0xE0E0FF);
    light.tertiary = Color::rgb(0xFFFFFF);
    light.error = Color::rgb(0xFFC0C0);
    light.on_surface_variant = Color::rgb(0xD0D0D0);
    every_colour_reads(light);
}

TEST_CASE("status colours: readable on every desktop palette, apart from each other") {
    using rummage::theme::status_colors;
    using rummage::theme::StatusRole;
    using rummage::theme::StatusScheme;
    for (const bool dark : {true, false}) {
        for (const std::uint32_t accent : kAccents) {
            CAPTURE(std::format("#{:06x} {}", accent, dark ? "dark" : "light"));
            const Palette palette = desktop_palette({.dark = dark, .accent = Color::rgb(accent)});
            for (const StatusScheme scheme : {StatusScheme::palette, StatusScheme::editor}) {
                CAPTURE(static_cast<int>(scheme));
                const auto colours = status_colors(palette, scheme);
                for (std::size_t i = 0; i < colours.size(); ++i) {
                    if (i == static_cast<std::size_t>(StatusRole::ignored)) {
                        continue;  // drawn faded on purpose
                    }
                    CAPTURE(i);
                    CHECK(contrast(colours.at(i), palette.surface) >= 4.5F);
                }
                const auto at = [&](StatusRole role) {
                    return colours.at(static_cast<std::size_t>(role));
                };
                // Modified, added, deleted, renamed and conflicts tell apart.
                CHECK_FALSE(same(at(StatusRole::modified), at(StatusRole::added)));
                CHECK_FALSE(same(at(StatusRole::added), at(StatusRole::deleted)));
                CHECK_FALSE(same(at(StatusRole::renamed), at(StatusRole::added)));
                CHECK_FALSE(same(at(StatusRole::conflict), at(StatusRole::deleted)));
                CHECK(same(at(StatusRole::untracked), at(StatusRole::added)));  // as editors do
            }
        }
    }
}

TEST_CASE("status colours: the palette scheme takes the palette's roles, the editor one its own") {
    using rummage::theme::status_colors;
    using rummage::theme::StatusRole;
    using rummage::theme::StatusScheme;
    const Palette palette;  // Noctalia's: every role reads on its surface as it is
    const auto at = [](const auto& colours, StatusRole role) {
        return colours.at(static_cast<std::size_t>(role));
    };
    const auto own = status_colors(palette, StatusScheme::palette);
    CHECK(same(at(own, StatusRole::modified), palette.primary));
    CHECK(same(at(own, StatusRole::added), palette.tertiary));
    CHECK(same(at(own, StatusRole::deleted), palette.error));
    CHECK(same(at(own, StatusRole::renamed), palette.secondary));
    CHECK_FALSE(same(at(own, StatusRole::conflict), at(own, StatusRole::deleted)));
    CHECK_FALSE(same(at(own, StatusRole::conflict),
                     at(status_colors(palette, StatusScheme::editor), StatusRole::conflict)));
    const auto editor = status_colors(palette, StatusScheme::editor);
    CHECK_FALSE(same(at(editor, StatusRole::modified), palette.primary));
    CHECK_FALSE(same(at(editor, StatusRole::added), palette.tertiary));
    CHECK(same(at(editor, StatusRole::deleted), palette.error));
    CHECK(status_colors(palette) == own);  // the default
}

TEST_CASE("status colours: a palette's own git colours are used as given") {
    using rummage::theme::status_colors;
    using rummage::theme::StatusRole;
    Palette palette;
    palette.git_modified = Color::rgb(0x123456);
    palette.git_conflict = Color::rgb(0xFF00FF);
    const auto colours = status_colors(palette);
    CHECK(same(colours.at(static_cast<std::size_t>(StatusRole::modified)), Color::rgb(0x123456)));
    CHECK(same(colours.at(static_cast<std::size_t>(StatusRole::conflict)), Color::rgb(0xFF00FF)));
    CHECK_FALSE(
        same(colours.at(static_cast<std::size_t>(StatusRole::added)), Color::rgb(0x123456)));
}
