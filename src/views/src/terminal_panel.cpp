#include "rummage/views/terminal_panel.hpp"

#include <algorithm>
#include <cmath>

#include "rummage/core/i18n.hpp"

namespace rummage::views {
TerminalPanel::TerminalPanel(core::EventLoop& loop, std::function<void()> changed)
    : changed_{std::move(changed)}, session_{loop, changed_} {}

float TerminalPanel::height(float available) const {
    return visible_ ? std::min(height_, std::max(0.0F, available - 100.0F)) : 0;
}

void TerminalPanel::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                         const theme::Palette& palette) {
    bounds_ = bounds;
    grip_ = {bounds.x, bounds.y, bounds.width, 6};
    close_ = {bounds.right() - 32, bounds.y + 6, 32, 24};
    renderer.fill_background(bounds, ui::Color::rgb(0x11131a));
    renderer.fill_rect(grip_, focused_ ? palette.primary : palette.outline_variant);
    renderer.draw_text(i18n::tr("Terminal · Ctrl+` focuses files"), bounds.x + 8, bounds.y + 8, 13,
                       palette.on_surface, bounds.width - 45);
    renderer.draw_text("×", close_.x + 8, close_.y, 18, palette.on_surface);
    constexpr float font = 14, row = 18;
    const float cell = std::max(1.0F, renderer.fonts()
                                              .layout("M", font, renderer.scale(), 0,
                                                      ui::Ellipsis::end, ui::Typeface::monospace)
                                              .width /
                                          renderer.scale());
    const auto area = ui::Rect{bounds.x + 6, bounds.y + 34, bounds.width - 12, bounds.height - 38};
    if (ui::AccessibleFrame::current()) {
        core::AccessibleNode node;
        node.id = "terminal";
        node.name = i18n::tr("Terminal");
        node.role = core::AccessibleRole::text;
        node.text = session_.text();
        node.focused = focused_;
        renderer.accessible(std::move(node), area);
    }
    session_.resize(static_cast<int>(area.height / row), static_cast<int>(area.width / cell));
    const auto state = session_.screen();
    renderer.push_clip(area);
    for (int y = 0; y < state.rows; ++y)
        for (int x = 0; x < state.columns; ++x) {
            const auto index = static_cast<std::size_t>(y * state.columns + x);
            if (index < state.cells.size())
                renderer.fill_rect({area.x + static_cast<float>(x) * cell,
                                    area.y + static_cast<float>(y) * row, cell, row},
                                   ui::Color::rgb(state.cells[index].bg));
        }

    for (int y = 0; y < state.rows; ++y)
        for (int x = 0; x < state.columns; ++x) {
            const auto index = static_cast<std::size_t>(y * state.columns + x);
            if (index >= state.cells.size())
                break;
            const auto& value = state.cells[index];
            const float px = area.x + static_cast<float>(x) * cell,
                        py = area.y + static_cast<float>(y) * row;

            if (!value.text.empty())
                renderer.draw_text(value.text, px, py, font, ui::Color::rgb(value.fg), 0,
                                   ui::Ellipsis::end, ui::Typeface::monospace);
            if (value.underline)
                renderer.fill_rect({px, py + row - 2, cell, 1}, ui::Color::rgb(value.fg));
        }
    if (focused_ && state.cursor_visible)
        renderer.fill_rect({area.x + static_cast<float>(state.cursor_column) * cell,
                            area.y + static_cast<float>(state.cursor_row) * row + row - 2, cell, 2},
                           palette.primary);
    renderer.pop_clip();
    if (!session_.error().empty())
        renderer.draw_text(session_.error(), bounds.x + 8, bounds.bottom() - 20, 13,
                           palette.on_surface, bounds.width - 16);
}

bool TerminalPanel::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    if (resizing_) {
        if (event.kind == Kind::motion) {
            height_ = std::clamp(resize_height_ + resize_y_ - event.y, 100.0F, 1000.0F);
            changed_();
        }
        if (event.kind == Kind::release)
            resizing_ = false;
        return true;
    }
    if (!visible_ || !bounds_.contains(event.x, event.y)) {
        if (event.kind == Kind::press)
            focused_ = false;
        return false;
    }
    if (event.kind == Kind::press) {
        focused_ = true;
        if (close_.contains(event.x, event.y)) {
            visible_ = focused_ = false;
            close_requested_ = true;
            changed_();
        } else if (grip_.contains(event.x, event.y)) {
            resizing_ = true;
            resize_y_ = event.y;
            resize_height_ = height_;
        }
    }
    if (event.kind == Kind::scroll)
        session_.scroll(
            static_cast<int>(event.wheel_y != 0 ? -event.wheel_y * 3 : -event.scroll_y / 18));
    changed_();
    return true;
}

void TerminalPanel::key(const ui::KeyEvent& event) {
    using Key = platform::EmbeddedTerminal::Key;
    Key key = Key::none;
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            key = Key::enter;
            break;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            key = Key::tab;
            break;
        case XKB_KEY_BackSpace:
            key = Key::backspace;
            break;
        case XKB_KEY_Escape:
            key = Key::escape;
            break;
        case XKB_KEY_Up:
            key = Key::up;
            break;
        case XKB_KEY_Down:
            key = Key::down;
            break;
        case XKB_KEY_Left:
            key = Key::left;
            break;
        case XKB_KEY_Right:
            key = Key::right;
            break;
        case XKB_KEY_Home:
            key = Key::home;
            break;
        case XKB_KEY_End:
            key = Key::end;
            break;
        case XKB_KEY_Insert:
            key = Key::insert;
            break;
        case XKB_KEY_Delete:
            key = Key::del;
            break;
        case XKB_KEY_Page_Up:
            key = Key::page_up;
            break;
        case XKB_KEY_Page_Down:
            key = Key::page_down;
            break;
        default:
            if (event.keysym >= XKB_KEY_F1 && event.keysym <= XKB_KEY_F12)
                key = static_cast<Key>(static_cast<int>(Key::f1) +
                                       static_cast<int>(event.keysym - XKB_KEY_F1));
            break;
    }
    if (event.modifiers.shift && (key == Key::page_up || key == Key::page_down)) {
        session_.scroll(key == Key::page_up ? 12 : -12);
        return;
    }
    auto text = event.text;
    if (event.modifiers.ctrl && event.keysym >= XKB_KEY_space && event.keysym <= XKB_KEY_asciitilde)
        text = std::string(1, static_cast<char>(event.keysym));
    if (key != Key::none || !text.empty())
        session_.key(key, text, event.modifiers.ctrl, event.modifiers.alt, event.modifiers.shift);
}
}  // namespace rummage::views
