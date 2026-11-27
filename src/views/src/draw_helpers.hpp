#pragma once

// Small text and chrome helpers shared by the views.

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
#include "rummage/ui/scroller.hpp"
#include "rummage/views/item_bounds.hpp"

namespace rummage::fs {
struct Entry;
}

namespace rummage::views {

class Folder;
class Thumbnails;
struct ItemStyle;

// A thumbnail fitted inside the icon square, or the file's normal icon while loading or
// unsupported. Passing no cache opts out without requesting work. Shared by all file views.
void draw_entry_icon(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                     const Folder& folder, const fs::Entry& entry, const ItemStyle& style,
                     Thumbnails* thumbnails, ui::Rect bounds, bool cut,
                     ItemVisual* painted = nullptr);

// Repaint the saved visual at a new position/size, without loading the source file again.
void draw_item_visual(ui::Renderer&, ui::IconSet&, const theme::Palette&, const ItemVisual&,
                      ui::Rect bounds);

// Height of the bar above the views (the breadcrumb).
inline constexpr float kTitleHeight = 40.0F;

// Reveal an indexed row, once its owner has resolved the cursor and a nonempty viewport.
void reveal_row(ui::Scroller& scroller, std::size_t row, float height, float viewport, bool jump,
                ui::Scroller::Clock::time_point now);

// Width of text in logical pixels.
float text_width(ui::Renderer& renderer, std::string_view text, float size);

// Text centered in bounds (with 12 px margins, ellipsized when longer), its top at y.
void draw_centered(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds, float y,
                   float size, ui::Color color);

void draw_right_aligned(ui::Renderer& renderer, std::string_view text, float right_x, float top_y,
                        float font_size, ui::Color color);

// Center a button label with 10 px side margins. Existing translated labels may separate
// a trailing shortcut with three spaces; keep that hint at the right and avoid overlap.
void draw_button_text(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds,
                      ui::Color color);

// The mark of a file that may run as a program (issue #17): a dot at the bottom right of its
// icon (the square at x, y, size wide), in dot, ringed in behind (the colour under it) so it
// stands apart from the icon on any theme. Its size follows the icon's, 6 to 12 px.
void draw_run_mark(ui::Renderer& renderer, float x, float y, float size, ui::Color dot,
                   ui::Color behind);

// A line's syntax colours as runs for Renderer::draw_text_runs, at opacity alpha.
std::vector<ui::ColorRun> colour_runs(std::span<const text::Span> spans,
                                      const theme::SyntaxColors& colours, float alpha);

// A mark on the bottom right of a file's icon drawn in the size x size box at (x, y), saying why
// its preview failed: a lock (password protected) or a cross (damaged). Nothing for other
// failures, which say nothing about the file.
void draw_failure_badge(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                        picture::Failure failure, float x, float y, float size, float alpha = 1.0F);

}  // namespace rummage::views
