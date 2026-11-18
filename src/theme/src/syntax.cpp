#include "rummage/theme/syntax.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <vector>

#include "oklch.hpp"
#include "rummage/theme/desktop.hpp"

namespace rummage::theme {
namespace {

// WCAG AA for normal text.
constexpr float kReadable = 4.5F;

// A green for added lines, about 143 degrees in OKLCH, as diff viewers use.
constexpr double kGreenHue = 2.5;
constexpr double kGreenChroma = 0.14;

// A role has a hue of its own from this chroma on (OKLCH); colours closer than kApart (in OKLab)
// look alike next to each other.
constexpr double kHued = 0.04;
constexpr double kApart = 0.08;

double distance(Lch a, Lch b) {
    const double da = (a.c * std::cos(a.h)) - (b.c * std::cos(b.h));
    const double db = (a.c * std::sin(a.h)) - (b.c * std::sin(b.h));
    return std::sqrt(((a.l - b.l) * (a.l - b.l)) + (da * da) + (db * db));
}

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

namespace {
std::atomic<StatusScheme> current_scheme{StatusScheme::palette};
}  // namespace

void set_status_scheme(StatusScheme scheme) noexcept {
    current_scheme.store(scheme, std::memory_order_relaxed);
}

StatusScheme status_scheme() noexcept { return current_scheme.load(std::memory_order_relaxed); }

StatusColors status_colors(const Palette& palette, StatusScheme scheme) {
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
    const auto role = [&](Color colour) { return readable(to_lch(colour), page); };
    // In StatusRole's order: added, modified, deleted, renamed, untracked, conflict, ignored
    // (drawn faded anyway).
    const Color green = hue(kGreenHue, 0.17);
    StatusColors colors{green, hue(kAmberHue, 0.13),   role(palette.error), hue(kTealHue, 0.10),
                        green, hue(kMagentaHue, 0.15), palette.outline};
    if (scheme == StatusScheme::palette) {
        // The palette's roles where they have a hue of their own and differ from the ones taken
        // before; otherwise (a grey accent, roles alike) the editor colour keeps states apart.
        std::vector<Color> taken{colors.at(static_cast<std::size_t>(StatusRole::deleted))};
        const auto take = [&](StatusRole role_of, Lch lch) {
            const bool apart = std::ranges::none_of(
                taken, [&](Color other) { return distance(lch, to_lch(other)) < kApart; });
            if (lch.c >= kHued && apart) {
                colors.at(static_cast<std::size_t>(role_of)) = readable(lch, page);
            }
            taken.push_back(colors.at(static_cast<std::size_t>(role_of)));
        };
        take(StatusRole::added, to_lch(palette.tertiary));
        take(StatusRole::modified, to_lch(palette.primary));
        take(StatusRole::renamed, to_lch(palette.secondary));
        // No role is for conflicts: halfway from the error colour to secondary, a red-violet.
        take(StatusRole::conflict, between(to_lch(palette.error), to_lch(palette.secondary)));
        colors.at(static_cast<std::size_t>(StatusRole::untracked)) =
            colors.at(static_cast<std::size_t>(StatusRole::added));
    }
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
