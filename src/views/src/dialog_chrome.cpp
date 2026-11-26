#include "rummage/views/dialog_chrome.hpp"

#include <algorithm>

#include "draw_helpers.hpp"
#include "rummage/views/selectable_text.hpp"

namespace rummage::views {

DialogFrame DialogFrame::fit(const ui::Rect& window, float width, float height,
                             float footer_height) {
    const float margin = window.width < 600 || window.height < 400 ? 8.0F : 24.0F;
    width = std::min(width, std::max(0.0F, window.width - 2 * margin));
    height = std::min(height, std::max(0.0F, window.height - 2 * margin));
    const ui::Rect card{window.x + (window.width - width) / 2,
                        window.y + (window.height - height) / 2, width, height};
    return fill(card, footer_height);
}

DialogFrame DialogFrame::fill(const ui::Rect& card, float footer_height) {
    const auto width = card.width, height = card.height;
    const float header = std::min(60.0F, height);
    footer_height = std::min(footer_height, height - header);
    return {card,
            {card.x, card.y, width, header},
            {card.x, card.y + header, width, height - header - footer_height},
            {card.x, card.bottom() - footer_height, width, footer_height}};
}

void DialogFrame::draw(ui::Renderer& renderer, const ui::Rect& window,
                       const theme::Palette& palette, std::string_view title,
                       bool window_content) const {
    if (window_content)
        renderer.fill_background(card, palette.surface_container_high);
    else
        draw_dialog_surface(renderer, window, card, palette);
    if (auto* frame = ui::AccessibleFrame::current())
        frame->dialog(std::string{title}, card);
    renderer.draw_text(title, header.x + 20, header.y + 18, 18, palette.on_surface,
                       std::max(1.0F, header.width - 40));
    renderer.fill_rect({card.x + 1, header.bottom(), std::max(0.0F, card.width - 2), 1},
                       palette.outline_variant);
    renderer.fill_rect({card.x + 1, footer.y, std::max(0.0F, card.width - 2), 1},
                       palette.outline_variant);
}

void draw_dialog_surface(ui::Renderer& renderer, const ui::Rect& window, const ui::Rect& card,
                         const theme::Palette& palette) {
    if (auto* frame = ui::AccessibleFrame::current())
        frame->dialog("Dialog", card);
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
    core::AccessibleNode node;
    node.id = "button:" + std::string{label} + ":" + std::to_string(static_cast<int>(bounds.y));
    node.name = label.substr(0, label.find("   "));
    node.role = core::AccessibleRole::button;
    node.focused = focused;
    renderer.accessible(std::move(node), bounds);
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

SettingRowLayout measure_setting_row(ui::Renderer& renderer, float width, std::string_view label,
                                     std::string_view description, float control_width,
                                     std::string_view control_label) {
    width = std::max(1.0F, width);
    const bool wide = width >= 500 && width - control_width - 18 >= 220;
    const float label_width = wide ? width - control_width - 18 : width;
    SettingRowLayout layout;
    layout.label = {0, 4, label_width,
                    SelectableText::height(renderer, label, label_width, 13, 20)};
    layout.description = {0, layout.label.bottom(), label_width,
                          description.empty()
                              ? 0
                              : SelectableText::height(renderer, description, label_width, 11, 16)};
    layout.control = wide ? ui::Rect{width - control_width, 5, control_width, 32}
                          : ui::Rect{0, layout.description.bottom() + 4, width, 32};
    if (!control_label.empty())
        layout.control.height = std::max(
            32.0F, SelectableText::height(renderer, control_label,
                                          std::max(1.0F, layout.control.width - 44), 13, 18) +
                       12);
    layout.height = std::max(
        {wide ? 64.0F : 94.0F, layout.description.bottom() + 20, layout.control.bottom() + 18});
    return layout;
}

ui::Rect draw_setting_row(ui::Renderer& renderer, const ui::Rect& row,
                          const theme::Palette& palette, std::string_view label,
                          std::string_view description, const SettingRowLayout& layout) {
    const auto place = [&](ui::Rect rect) {
        rect.x += row.x;
        rect.y += row.y;
        return rect;
    };
    SelectableText text;
    text.begin_frame(row);
    text.draw(renderer, "label", label, place(layout.label), 13, palette.on_surface, {}, 20);
    if (!description.empty())
        text.draw(renderer, "description", description, place(layout.description), 11,
                  palette.on_surface_variant, {}, 16);
    return place(layout.control);
}

}  // namespace rummage::views
