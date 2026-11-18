#include "rummage/views/dialog_chrome.hpp"

#include <algorithm>

#include "draw_helpers.hpp"

namespace rummage::views {

DialogFrame DialogFrame::fit(const ui::Rect& window, float width, float height,
                             float footer_height) {
    const float margin = window.width < 600 || window.height < 400 ? 8.0F : 24.0F;
    width = std::min(width, std::max(0.0F, window.width - 2 * margin));
    height = std::min(height, std::max(0.0F, window.height - 2 * margin));
    const ui::Rect card{window.x + (window.width - width) / 2,
                        window.y + (window.height - height) / 2, width, height};
    const float header = std::min(60.0F, height);
    footer_height = std::min(footer_height, height - header);
    return {card,
            {card.x, card.y, width, header},
            {card.x, card.y + header, width, height - header - footer_height},
            {card.x, card.bottom() - footer_height, width, footer_height}};
}

void DialogFrame::draw(ui::Renderer& renderer, const ui::Rect& window,
                       const theme::Palette& palette, std::string_view title) const {
    draw_dialog_surface(renderer, window, card, palette);
    renderer.draw_text(title, header.x + 20, header.y + 18, 18, palette.on_surface,
                       std::max(1.0F, header.width - 40));
    renderer.fill_rect({card.x + 1, header.bottom(), std::max(0.0F, card.width - 2), 1},
                       palette.outline_variant);
    renderer.fill_rect({card.x + 1, footer.y, std::max(0.0F, card.width - 2), 1},
                       palette.outline_variant);
}

void draw_dialog_surface(ui::Renderer& renderer, const ui::Rect& window, const ui::Rect& card,
                         const theme::Palette& palette) {
    renderer.fill_rect(window, palette.scrim.with_alpha(0.55F));
    renderer.draw_shadow(card, ui::Corners::all(8), 16, palette.shadow.with_alpha(0.5F));
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(8),
                             .border_width = 1,
                             .border = palette.outline_variant});
}

void draw_dialog_button(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, std::string_view label, bool focused,
                        DialogButton style) {
    const auto fill = style == DialogButton::primary       ? palette.primary
                      : style == DialogButton::destructive ? palette.error
                                                           : palette.surface_container_highest;
    const auto ink = style == DialogButton::primary       ? palette.on_primary
                     : style == DialogButton::destructive ? palette.on_error
                                                          : palette.on_surface;
    renderer.draw_box(bounds, {.fill = fill,
                               .radius = ui::Corners::all(4),
                               .border_width = focused ? 2.0F : 0.0F,
                               .border = palette.on_surface});
    draw_button_text(renderer, label, bounds, ink);
}

ui::Rect draw_setting_row(ui::Renderer& renderer, const ui::Rect& row,
                          const theme::Palette& palette, std::string_view label,
                          std::string_view description) {
    const bool wide = row.width >= 500;
    const float label_width = wide ? row.width - 218 : row.width;
    renderer.draw_text(label, row.x, row.y + 4, 13, palette.on_surface,
                       std::max(1.0F, label_width));
    if (!description.empty())
        renderer.draw_text(description, row.x, row.y + 24, 11, palette.on_surface_variant,
                           std::max(1.0F, label_width));
    return wide ? ui::Rect{row.right() - 200, row.y + 5, 200, 32}
                : ui::Rect{row.x, row.y + 44, row.width, 32};
}

}  // namespace rummage::views
