#pragma once

#include <optional>

#include "rummage/theme/palette.hpp"

// Generated palettes use light/dark and an accent. The Settings portal supplies these
// preferences without a palette file; partial palette files use the same builder for defaults.
namespace rummage::theme {

struct Desktop {
    bool dark{false};
    std::optional<Color> accent;  // opaque; none: the desktop has no accent colour

    bool operator==(const Desktop&) const = default;
};

// GNOME's default blue, for desktops that give no accent.
inline constexpr Color kDefaultAccent = Color::rgb(0x3584E4);

// Neutral surfaces with a trace of the accent's hue, the accent for the primary roles, its
// lightness moved where text and icons stay readable on those surfaces (OKLCH tones).
[[nodiscard]] Palette desktop_palette(const Desktop& desktop);

// WCAG 2 contrast ratio of two opaque colours, 1 to 21.
[[nodiscard]] float contrast(Color a, Color b);

}  // namespace rummage::theme
