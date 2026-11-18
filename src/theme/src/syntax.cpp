#include "rummage/theme/syntax.hpp"

#include <cmath>
#include <numbers>

#include "oklch.hpp"
#include "rummage/theme/desktop.hpp"

namespace rummage::theme {
namespace {

// WCAG AA for normal text.
constexpr float kReadable = 4.5F;

// A green for added lines, about 143 degrees in OKLCH, as diff viewers use.
constexpr double kGreenHue = 2.5;
constexpr double kGreenChroma = 0.14;

// Halfway between two colours in OKLCH: lightness and chroma averaged, the hue along the shorter
// arc.
Lch between(Lch a, Lch b) {
    double turn = b.h - a.h;
    if (turn > std::numbers::pi) {
        turn -= 2.0 * std::numbers::pi;
    } else if (turn < -std::numbers::pi) {
        turn += 2.0 * std::numbers::pi;
    }
    return {(a.l + b.l) / 2.0, (a.c + b.c) / 2.0, a.h + (turn / 2.0)};
}

// colour, or the nearest one in lightness (hue and chroma kept as far as sRGB allows) that reads
// on background; away from the background: lighter on a dark one, darker on a light one.
Color readable(Lch colour, Color background) {
    Color shown = from_lch(colour);
    const double step = to_lch(background).l < 0.6 ? 0.01 : -0.01;
    for (int i = 0; i < 100 && contrast(shown, background) < kReadable; ++i) {
        colour.l += step;
        shown = from_lch(colour);
    }
    return shown;
}

}  // namespace

SyntaxColors syntax_colors(const Palette& palette) {
    const Color page = palette.surface_container.with_alpha(1.0F);
    const Lch primary = to_lch(palette.primary);
    const Lch secondary = to_lch(palette.secondary);
    const Lch tertiary = to_lch(palette.tertiary);
    const Lch error = to_lch(palette.error);
    return {
        readable(primary, page),                                              // keyword
        readable(tertiary, page),                                             // type
        readable(secondary, page),                                            // string
        readable(between(primary, error), page),                              // number
        readable(to_lch(palette.on_surface_variant), page),                   // comment
        readable(between(secondary, tertiary), page),                         // function
        readable({.l = primary.l, .c = kGreenChroma, .h = kGreenHue}, page),  // added
        readable(error, page),                                                // removed
    };
}

StatusColors status_colors(const Palette& palette) {
    const Color page = palette.surface.with_alpha(1.0F);
    // A fixed, middle lightness as editors' colours have (not the accent's, which may be nearly
    // white or black), lighter on a dark page; hues in OKLCH radians: amber, blue-green, magenta.
    const double lightness = to_lch(page).l < 0.6 ? 0.80 : 0.52;
    constexpr double kAmberHue = 1.31;
    constexpr double kTealHue = 3.49;
    constexpr double kMagentaHue = 5.76;
    const auto hue = [&](double h, double c) {
        return readable({.l = lightness, .c = c, .h = h}, page);
    };
    const Color green = hue(kGreenHue, 0.17);
    StatusColors colors{
        green,                                  // added
        hue(kAmberHue, 0.13),                   // modified
        readable(to_lch(palette.error), page),  // deleted
        hue(kTealHue, 0.10),                    // renamed
        green,                                  // untracked
        hue(kMagentaHue, 0.15),                 // conflict
        palette.outline,                        // ignored (drawn faded anyway)
    };
    const std::array overrides{palette.git_added,   palette.git_modified,  palette.git_deleted,
                               palette.git_renamed, palette.git_untracked, palette.git_conflict,
                               palette.git_ignored};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        if (overrides.at(i)) {
            colors.at(i) = *overrides.at(i);
        }
    }
    return colors;
}

}  // namespace rummage::theme
