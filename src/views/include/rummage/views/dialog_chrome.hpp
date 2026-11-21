#pragma once

#include <string_view>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"

namespace rummage::views {

// Shared geometry and painting; each dialog owns its focus, scrolling and dismissal policy.
struct DialogFrame {
    ui::Rect card, header, body, footer;
    static DialogFrame fit(const ui::Rect& window, float width, float height,
                           float footer_height = 56.0F);
    void draw(ui::Renderer& renderer, const ui::Rect& window, const theme::Palette& palette,
              std::string_view title) const;
};

// Shared surface for dialogs with specialized body geometry.
void draw_dialog_surface(ui::Renderer& renderer, const ui::Rect& window, const ui::Rect& card,
                         const theme::Palette& palette);

enum class DialogButton { normal, primary, destructive };
void draw_dialog_button(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, std::string_view label, bool focused,
                        DialogButton style = DialogButton::normal);

// Measured in row-local coordinates. Narrow rows stack the control below wrapped text.
struct SettingRowLayout {
    ui::Rect label, description, control;
    float height{94};
};

SettingRowLayout measure_setting_row(ui::Renderer& renderer, float width, std::string_view label,
                                     std::string_view description, float control_width = 200,
                                     std::string_view control_label = {});
ui::Rect draw_setting_row(ui::Renderer& renderer, const ui::Rect& row,
                          const theme::Palette& palette, std::string_view label,
                          std::string_view description, const SettingRowLayout& layout);

}  // namespace rummage::views
