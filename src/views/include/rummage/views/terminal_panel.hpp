#pragma once
#include <utility>

#include "rummage/platform/embedded_terminal.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {
class TerminalPanel {
public:
    TerminalPanel(core::EventLoop& loop, std::function<void()> changed);

    platform::EmbeddedTerminal& session() { return session_; }

    const platform::EmbeddedTerminal& session() const { return session_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);
    bool pointer(const ui::PointerEvent& event);
    void key(const ui::KeyEvent& event);

    bool focused() const { return focused_; }

    void focus(bool value) { focused_ = value; }

    bool visible() const { return visible_; }

    void show(bool value) {
        visible_ = value;
        focused_ = value;
        changed_();
    }

    float height(float available) const;

    const ui::Rect& bounds() const { return bounds_; }

    bool close_requested() { return std::exchange(close_requested_, false); }

private:
    std::function<void()> changed_;
    platform::EmbeddedTerminal session_;
    bool visible_{false}, focused_{false}, resizing_{false}, close_requested_{false};
    float height_{260}, resize_y_{}, resize_height_{};
    ui::Rect bounds_{}, close_{}, grip_{};
};
}  // namespace rummage::views
