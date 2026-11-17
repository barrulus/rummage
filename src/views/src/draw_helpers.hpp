#pragma once

// Small text and chrome helpers shared by the views.

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/syntax.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/views/decorations.hpp"

namespace rummage::views {

// Height of the bar above the views (the breadcrumb).
inline constexpr float kTitleHeight = 40.0F;

// Width of text in logical pixels.
float text_width(ui::Renderer& renderer, std::string_view text, float size);

// Text centered in bounds (with 12 px margins, ellipsized when longer), its top at y.
void draw_centered(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds, float y,
                   float size, ui::Color color);

void draw_right_aligned(ui::Renderer& renderer, std::string_view text, float right_x, float top_y,
                        float font_size, ui::Color color);

// The mark of a file that may run as a program (issue #17): a dot at the bottom right of its
// icon (the square at x, y, size wide), in dot, ringed in behind (the colour under it) so it
// stands apart from the icon on any theme. Its size follows the icon's, 6 to 12 px.
void draw_run_mark(ui::Renderer& renderer, float x, float y, float size, ui::Color dot,
                   ui::Color behind);

// A decorator's badge: its text in its tone (bold when strong), centered in a box about one
// letter wide whose right edge is at right and top at y. on: the item's text colour when its fill
// is the strong selection, which the tones would not stand out on. Returns the box's width; 0 for
// a badge without text (one that only fades its entry).
float draw_badge(ui::Renderer& renderer, const theme::Palette& palette, const Badge& badge,
                 float right, float y, float size, std::optional<ui::Color> on);

// A line's syntax colours as runs for Renderer::draw_text_runs, at opacity alpha.
std::vector<ui::ColorRun> colour_runs(std::span<const text::Span> spans,
                                      const theme::SyntaxColors& colours, float alpha);

// A mark on the bottom right of a file's icon drawn in the size x size box at (x, y), saying why
// its preview failed: a lock (password protected) or a cross (damaged). Nothing for other
// failures, which say nothing about the file.
void draw_failure_badge(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                        picture::Failure failure, float x, float y, float size, float alpha = 1.0F);

}  // namespace rummage::views
