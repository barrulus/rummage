#include "rummage/views/terminal_panel.hpp"

#include <algorithm>
#include <cmath>

#include "rummage/core/i18n.hpp"

namespace rummage::views {
TerminalPanel::TerminalPanel(core::EventLoop& loop, std::function<void()> changed)
    : changed_{std::move(changed)}, session_{loop, changed_} {}

void TerminalPanel::set_layout(TerminalLayout layout) {
    layout.width = std::clamp(layout.width, 200.0F, 2400.0F);
    layout.height = std::clamp(layout.height, 100.0F, 2000.0F);
    layout.x = std::clamp(layout.x, 0.0F, 10000.0F);
    layout.y = std::clamp(layout.y, 0.0F, 10000.0F);
    layout_ = layout;
    dragging_ = resizing_ = false;
    changed_();
}

void TerminalPanel::set_placement(TerminalPlacement placement) {
    auto next = layout_;
    next.placement = placement;
    set_layout(next);
}

float TerminalPanel::height(float available) const {
    return visible_ ? std::min(layout_.height, std::max(0.0F, available - 100.0F)) : 0;
}

ui::Rect TerminalPanel::arrange(const ui::Rect& available) {
    available_ = available;
    bounds_ = {};
    if (!visible_)
        return available;
    bounds_ = available;
    switch (layout_.placement) {
        case TerminalPlacement::bottom:
        case TerminalPlacement::top:
            bounds_.height = height(available.height);
            if (layout_.placement == TerminalPlacement::bottom)
                bounds_.y = available.bottom() - bounds_.height;
            break;
        case TerminalPlacement::left:
        case TerminalPlacement::right:
            bounds_.width = std::min(layout_.width, std::max(0.0F, available.width - 100));
            if (layout_.placement == TerminalPlacement::right)
                bounds_.x = available.right() - bounds_.width;
            break;
        case TerminalPlacement::floating:
            bounds_.width = std::min(layout_.width, std::max(0.0F, available.width));
            bounds_.height = std::min(layout_.height, std::max(0.0F, available.height));
            bounds_.x +=
                std::clamp(layout_.x, 0.0F, std::max(0.0F, available.width - bounds_.width));
            bounds_.y +=
                std::clamp(layout_.y, 0.0F, std::max(0.0F, available.height - bounds_.height));
            break;
    }
    controls();
    return remaining(available);
}

ui::Rect TerminalPanel::remaining(ui::Rect bounds) const {
    if (!visible_ || bounds_.empty())
        return bounds;
    switch (layout_.placement) {
        case TerminalPlacement::bottom:
            bounds.height = std::max(0.0F, bounds_.y - bounds.y);
            break;
        case TerminalPlacement::top:
            bounds.height = std::max(0.0F, bounds.bottom() - bounds_.bottom());
            bounds.y = bounds_.bottom();
            break;
        case TerminalPlacement::left:
            bounds.width = std::max(0.0F, bounds.right() - bounds_.right());
            bounds.x = bounds_.right();
            break;
        case TerminalPlacement::right:
            bounds.width = std::max(0.0F, bounds_.x - bounds.x);
            break;
        case TerminalPlacement::floating:
            break;
    }
    return bounds;
}

void TerminalPanel::controls() {
    header_ = {bounds_.x, bounds_.y, bounds_.width, std::min(34.0F, bounds_.height)};
    close_ = {bounds_.right() - 32, bounds_.y + 6, 28, 24};
    switch (layout_.placement) {
        case TerminalPlacement::bottom:
            grip_ = {bounds_.x, bounds_.y, bounds_.width, 6};
            break;
        case TerminalPlacement::top:
            grip_ = {bounds_.x, bounds_.bottom() - 6, bounds_.width, 6};
            break;
        case TerminalPlacement::left:
            grip_ = {bounds_.right() - 6, bounds_.y, 6, bounds_.height};
            break;
        case TerminalPlacement::right:
            grip_ = {bounds_.x, bounds_.y, 6, bounds_.height};
            break;
        case TerminalPlacement::floating:
            grip_ = {bounds_.right() - 14, bounds_.bottom() - 14, 14, 14};
            break;
    }
}

bool TerminalPanel::claims(float x, float y) const {
    return visible_ && (resizing_ || dragging_ || bounds_.contains(x, y));
}

ui::CursorShape TerminalPanel::cursor_at(float x, float y) const {
    using Shape = ui::CursorShape;
    if (dragging_)
        return Shape::grabbing;
    if (resizing_ || grip_.contains(x, y)) {
        if (layout_.placement == TerminalPlacement::floating)
            return Shape::se_resize;
        return layout_.placement == TerminalPlacement::left ||
                       layout_.placement == TerminalPlacement::right
                   ? Shape::ew_resize
                   : Shape::ns_resize;
    }
    if (close_.contains(x, y))
        return Shape::pointer;
    if (layout_.placement == TerminalPlacement::floating && header_.contains(x, y))
        return Shape::grab;
    return Shape::standard;
}

void TerminalPanel::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                         const theme::Palette& palette) {
    bounds_ = bounds;
    controls();
    if (bounds.empty())
        return;
    renderer.push_clip(bounds);
    renderer.fill_background(bounds, ui::Color::rgb(0x11131a));
    renderer.fill_rect(header_, palette.surface_container_highest);
    renderer.fill_rect(grip_, focused_ ? palette.primary : palette.outline_variant);
    renderer.draw_text(
        bounds.width >= 360 ? i18n::tr("Terminal · Ctrl+` focuses files") : i18n::tr("Terminal"),
        bounds.x + 8, bounds.y + 8, 13, palette.on_surface, std::max(0.0F, bounds.width - 45));
    renderer.draw_text("×", close_.x + 8, close_.y, 18, palette.on_surface);
    constexpr float font = 14, row = 18;
    const float cell = std::max(1.0F, renderer.fonts()
                                              .layout("M", font, renderer.scale(), 0,
                                                      ui::Ellipsis::end, ui::Typeface::monospace)
                                              .width /
                                          renderer.scale());
    const auto area = ui::Rect{bounds.x + 6, bounds.y + 34, std::max(0.0F, bounds.width - 12),
                               std::max(0.0F, bounds.height - 38)};
    session_.resize(static_cast<int>(area.height / row), static_cast<int>(area.width / cell));
    if (ui::AccessibleFrame::current()) {
        core::AccessibleNode node;
        node.id = "terminal";
        node.name = i18n::tr("Terminal");
        node.role = core::AccessibleRole::text;
        node.text = session_.text();
        node.focused = focused_;
        renderer.accessible(std::move(node), area);
    }
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
    renderer.pop_clip();
}

bool TerminalPanel::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    if (resizing_ || dragging_) {
        if (event.kind == Kind::motion) {
            const auto dx = event.x - drag_x_, dy = event.y - drag_y_;
            if (dragging_) {
                layout_.x = std::clamp(drag_layout_.x + dx, 0.0F,
                                       std::max(0.0F, available_.width - bounds_.width));
                layout_.y = std::clamp(drag_layout_.y + dy, 0.0F,
                                       std::max(0.0F, available_.height - bounds_.height));
            } else {
                if (layout_.placement == TerminalPlacement::left ||
                    layout_.placement == TerminalPlacement::right ||
                    layout_.placement == TerminalPlacement::floating)
                    layout_.width =
                        std::clamp(drag_layout_.width +
                                       (layout_.placement == TerminalPlacement::right ? -dx : dx),
                                   200.0F, 2400.0F);
                if (layout_.placement == TerminalPlacement::top ||
                    layout_.placement == TerminalPlacement::bottom ||
                    layout_.placement == TerminalPlacement::floating)
                    layout_.height =
                        std::clamp(drag_layout_.height +
                                       (layout_.placement == TerminalPlacement::bottom ? -dy : dy),
                                   100.0F, 2000.0F);
            }
            changed_();
        }
        if (event.kind == Kind::release)
            resizing_ = dragging_ = false;
        return true;
    }
    if (!visible_ || !bounds_.contains(event.x, event.y)) {
        if (event.kind == Kind::press)
            focused_ = false;
        return false;
    }
    if (event.kind == Kind::press) {
        focused_ = true;
        if (event.button != ui::PointerButton::left && event.button != ui::PointerButton::other) {
            changed_();
            return true;
        }
        if (close_.contains(event.x, event.y)) {
            visible_ = focused_ = false;
            close_requested_ = true;
            changed_();
        } else if (grip_.contains(event.x, event.y) ||
                   (layout_.placement == TerminalPlacement::floating &&
                    header_.contains(event.x, event.y))) {
            resizing_ = grip_.contains(event.x, event.y);
            dragging_ = !resizing_;
            drag_x_ = event.x;
            drag_y_ = event.y;
            drag_layout_ = layout_;
            // Start from the rendered geometry after a window was made smaller.
            if (!available_.empty()) {
                drag_layout_.width = bounds_.width;
                drag_layout_.height = bounds_.height;
                drag_layout_.x = bounds_.x - available_.x;
                drag_layout_.y = bounds_.y - available_.y;
            }
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
