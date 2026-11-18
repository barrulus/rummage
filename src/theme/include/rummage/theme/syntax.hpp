#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rummage/theme/palette.hpp"

// The colours of source code in Quick Look and the info pane (ROADMAP 3b-2), made from the
// palette so that a new theme recolours the code too: keywords in the primary colour, types in
// the tertiary, strings in the secondary, comments in on_surface_variant, removed lines in the
// error colour; numbers, functions and added lines from hues between those (OKLCH). Every colour
// is moved in lightness until it reads on surface_container (WCAG AA, 4.5:1), the page the text
// is drawn on.
namespace rummage::theme {

// In text::Style's order: keyword, type, string, number, comment, function, added, removed.
inline constexpr std::size_t kSyntaxColors = 8;
using SyntaxColors = std::array<Color, kSyntaxColors>;

[[nodiscard]] SyntaxColors syntax_colors(const Palette& palette);

// The colours of a file's state (in git's sense). StatusScheme::palette takes the palette's own
// roles: modified primary, added and untracked tertiary, deleted error, renamed secondary,
// conflicts halfway between error and secondary, ignored outline; a role without a hue of its own
// or too like one before it (a grey accent) takes the editor colour instead. StatusScheme::editor
// uses editors' (VS Code's) hues at a middle lightness (lighter on dark pages): modified amber,
// added and untracked green, deleted the error colour, renamed blue-green, conflicts magenta.
// Either is moved in lightness until it reads on surface (WCAG AA); a palette's git_* roles win
// over both.
enum class StatusScheme : std::uint8_t { palette, editor };
enum class StatusRole : std::uint8_t {
    added,
    modified,
    deleted,
    renamed,
    untracked,
    conflict,
    ignored
};
inline constexpr std::size_t kStatusColors = 7;
using StatusColors = std::array<Color, kStatusColors>;

[[nodiscard]] StatusColors status_colors(const Palette& palette,
                                         StatusScheme scheme = StatusScheme::palette);

// The scheme the views draw file states in (set from the settings; palette until then).
void set_status_scheme(StatusScheme scheme) noexcept;
[[nodiscard]] StatusScheme status_scheme() noexcept;

}  // namespace rummage::theme
