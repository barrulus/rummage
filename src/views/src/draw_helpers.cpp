#include "draw_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "item_style.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

void draw_entry_icon(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                     const Folder& folder, const fs::Entry& entry, const ItemStyle& style,
                     Thumbnails* thumbnails, ui::Rect bounds, bool cut, ItemVisual* painted) {
    fs::Entry typed = entry;
    typed.mime = folder.mime_of(entry);
    const auto& mime = folder.mime();
    const auto type = mime.name(typed.mime);
    const bool video = type.starts_with("video/");
    const float size = bounds.width;
    ItemVisual visual;
    visual.bounds = bounds;
    visual.mime = type;
    visual.icon = fs::icon_name(mime, typed);
    visual.folder_icon = typed.folder_icon;
    visual.color = style.icon;
    visual.opacity = cut ? kCutOpacity : 1.0F;
    visual.rail = video ? std::clamp(size * 0.045F, 3.0F, 12.0F) : 0.0F;
    visual.executable = folder.runs_as_program(entry);
    visual.run_color = style.fill ? style.icon
                                  : theme::readable_foreground(palette.primary, palette.surface)
                                        .with_alpha(visual.opacity);
    visual.run_background = style.fill.value_or(palette.surface);
    if (thumbnails && !entry.is_directory() && !folder.is_archive() &&
        !folder.path_of(entry).empty()) {
        const auto registry = engines::installed();
        const auto* family = registry->find(mime, typed.mime);
        if (video || (family && family->thumbnails)) {
            visual.thumbnail = thumbnails->get_shared(
                folder.path_of(entry), entry.modified_ns,
                thumbs::size_for(static_cast<std::uint32_t>(std::lround(size * renderer.scale()))),
                type, &visual.failure, !entry.can_preview());
        }
    }
    draw_item_visual(renderer, icons, palette, visual, bounds);
    if (painted) {
        visual.path = folder.path_of(entry);
        *painted = std::move(visual);
    }
}

void draw_item_visual(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                      const ItemVisual& visual, ui::Rect bounds) {
    const float size = bounds.width;
    const float alpha = visual.opacity;
    if (visual.thumbnail) {
        const auto& thumbnail = *visual.thumbnail;
        // Scale the same film decoration along with the retained pixels.
        const float rail = visual.rail * size / visual.bounds.width;
        const float fit = std::min((size - 2 * rail) / static_cast<float>(thumbnail.width()),
                                   bounds.height / static_cast<float>(thumbnail.height()));
        const float width = static_cast<float>(thumbnail.width()) * fit;
        const float height = static_cast<float>(thumbnail.height()) * fit;
        const ui::Rect frame{bounds.x + (size - width) / 2, bounds.y + (bounds.height - height) / 2,
                             width, height};
        renderer.draw_image(thumbnail, frame, {}, alpha);
        if (rail > 0) {
            for (const float edge : {frame.x - rail, frame.right()}) {
                renderer.fill_rect({edge, frame.y, rail, frame.height},
                                   ui::Color::rgb(0x222222).with_alpha(alpha));
                if (visual.bounds.width >= 48) {
                    const float hole = rail * 0.5F;
                    for (float top = frame.y + hole; top + hole <= frame.bottom();
                         top += rail * 1.5F)
                        renderer.fill_rect({edge + rail * 0.25F, top, hole, hole},
                                           ui::Color::rgb(0xdddddd).with_alpha(alpha));
                }
            }
        }
    } else {
        icons.draw_file(renderer, visual.mime, visual.icon, bounds.x, bounds.y, size, visual.color,
                        visual.folder_icon);
        draw_failure_badge(renderer, icons, palette, visual.failure, bounds.x, bounds.y, size,
                           alpha);
    }
    if (visual.executable)
        draw_run_mark(renderer, bounds.x, bounds.y, size, visual.run_color, visual.run_background);
}

float text_width(ui::Renderer& renderer, std::string_view text, float size) {
    return renderer.fonts().layout(text, size, renderer.scale()).width / renderer.scale();
}

void draw_centered(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds, float y,
                   float size, ui::Color color) {
    const float max = bounds.width - 24.0F;
    const float width = std::min(text_width(renderer, text, size), max);
    renderer.draw_text(text, bounds.x + ((bounds.width - width) / 2.0F), y, size, color,
                       std::max(1.0F, max), ui::Ellipsis::middle);
}

void draw_right_aligned(ui::Renderer& renderer, std::string_view text, float right_x, float top_y,
                        float font_size, ui::Color color) {
    renderer.draw_text(text, right_x - text_width(renderer, text, font_size), top_y, font_size,
                       color);
}

void draw_button_text(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds,
                      ui::Color color) {
    const ui::Rect content{bounds.x + 10, bounds.y, bounds.width - 20, bounds.height};
    if (content.empty())
        return;
    const float scale = renderer.scale();
    float label_right = content.right();
    renderer.push_clip(content);
    const auto separator = text.rfind("   ");
    if (separator != std::string_view::npos) {
        const auto& hint =
            renderer.fonts().layout(text.substr(separator + 3), 13, scale, content.width);
        const float x = content.right() - hint.width / scale;
        renderer.draw_layout(
            hint, x, content.y + (content.height - (hint.ascent + hint.descent) / scale) / 2,
            color);
        label_right = x - 8;
        text = text.substr(0, separator);
    }
    const float available = label_right - content.x;
    if (available > 0) {
        const auto& label = renderer.fonts().layout(text, 13, scale, available);
        const float width = label.width / scale;
        // Keep short labels centered on the button; move longer ones just enough to
        // leave a gap before the shortcut. Ellipsize before measuring the final position.
        const float x = std::max(
            content.x, std::min(bounds.x + (bounds.width - width) / 2, label_right - width));
        renderer.draw_layout(
            label, x, content.y + (content.height - (label.ascent + label.descent) / scale) / 2,
            color);
    }
    renderer.pop_clip();
}

std::vector<ui::ColorRun> colour_runs(std::span<const text::Span> spans,
                                      const theme::SyntaxColors& colours, float alpha) {
    std::vector<ui::ColorRun> runs;
    runs.reserve(spans.size());
    for (const text::Span& span : spans) {
        runs.push_back(
            {.start = span.start,
             .end = span.start + span.length,
             .color = colours.at(static_cast<std::size_t>(span.style)).with_alpha(alpha)});
    }
    return runs;
}

void draw_failure_badge(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                        picture::Failure failure, float x, float y, float size, float alpha) {
    const bool locked = failure == picture::Failure::locked;
    if (!locked && failure != picture::Failure::damaged) {
        return;
    }
    const float badge = std::max(12.0F, size * 0.42F);
    const float ring = std::max(1.5F, badge * 0.08F);
    const float left = x + size - (badge * 0.85F);
    const float top = y + size - (badge * 0.85F);
    // A disc in the surface colour first, so the mark stands out on any icon.
    renderer.draw_box({left - ring, top - ring, badge + (2.0F * ring), badge + (2.0F * ring)},
                      {.fill = palette.surface.with_alpha(alpha),
                       .radius = ui::Corners::all((badge / 2.0F) + ring)});
    if (locked) {
        renderer.draw_box({left, top, badge, badge}, {.fill = palette.primary.with_alpha(alpha),
                                                      .radius = ui::Corners::all(badge / 2.0F)});
        const float glyph = badge * 0.62F;
        icons.draw(renderer, "lock-filled", left + ((badge - glyph) / 2.0F),
                   top + ((badge - glyph) / 2.0F), glyph, palette.on_primary.with_alpha(alpha));
    } else {
        icons.draw(renderer, "circle-x-filled", left, top, badge, palette.error.with_alpha(alpha));
    }
}

void draw_run_mark(ui::Renderer& renderer, float x, float y, float size, ui::Color dot,
                   ui::Color behind) {
    const float ring = 1.5F;
    const float diameter = std::clamp(size * 0.32F, 6.0F, 12.0F);
    // Its centre a quarter of itself inside the icon's corner: on the edge of the glyph, not on it.
    const float centre_x = x + size - (diameter / 4.0F);
    const float centre_y = y + size - (diameter / 4.0F);
    const float outer = diameter + (2.0F * ring);
    renderer.draw_box({centre_x - (outer / 2.0F), centre_y - (outer / 2.0F), outer, outer},
                      {.fill = dot,
                       .radius = ui::Corners::all(outer / 2.0F),
                       .border_width = ring,
                       .border = behind});
}

}  // namespace rummage::views
