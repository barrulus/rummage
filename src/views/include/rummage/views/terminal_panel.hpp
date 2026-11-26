#pragma once
#include <utility>

#include "rummage/platform/embedded_terminal.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/terminal_layout.hpp"

namespace rummage::views {
class TerminalPanel {
public:
    TerminalPanel(core::EventLoop& loop, std::function<void()> changed);

    platform::EmbeddedTerminal& session() { return session_; }

    const platform::EmbeddedTerminal& session() const { return session_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);
    bool pointer(const ui::PointerEvent& event);
    void key(const ui::KeyEvent& event);
    ui::Rect arrange(const ui::Rect& available);
    [[nodiscard]] ui::Rect remaining(ui::Rect bounds) const;
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    [[nodiscard]] bool claims(float x, float y) const;

    [[nodiscard]] const TerminalLayout& layout() const { return layout_; }

    void set_layout(TerminalLayout layout);
    void set_placement(TerminalPlacement placement);

    bool focused() const { return focused_; }

    void focus(bool value) { focused_ = value; }

    bool visible() const { return visible_; }

    void show(bool value) {
        visible_ = value;
        focused_ = value;
        dragging_ = resizing_ = false;
        changed_();
    }

    float height(float available) const;

    const ui::Rect& bounds() const { return bounds_; }

    bool close_requested() { return std::exchange(close_requested_, false); }

private:
    std::function<void()> changed_;
    platform::EmbeddedTerminal session_;
    void controls();
    bool visible_{false}, focused_{false}, resizing_{false}, dragging_{false},
        close_requested_{false};
    TerminalLayout layout_{}, drag_layout_{};
    float drag_x_{}, drag_y_{};
    ui::Rect available_{}, bounds_{}, close_{}, grip_{}, header_{};
};
}  // namespace rummage::views
