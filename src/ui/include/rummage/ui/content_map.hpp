#pragma once

#include "rummage/ui/clipboard.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::ui {

// Where the window's content is in its surface: past the frame's margin (offset, surface logical
// pixels) and zoomed by the interface's size ([font] size): one content pixel is zoom surface
// pixels. The views work in content pixels; the compositor speaks surface pixels.
struct ContentMap {
    float offset{0.0F};
    float zoom{1.0F};

    [[nodiscard]] constexpr float to_content(float surface) const noexcept {
        return (surface - offset) / zoom;
    }

    [[nodiscard]] constexpr float to_surface(float content) const noexcept {
        return (content * zoom) + offset;
    }

    // A distance (a scroll, a band's width) in content pixels.
    [[nodiscard]] constexpr float length(float surface) const noexcept { return surface / zoom; }

    // The event where the views see it: the position and a touchpad's scroll (so the content
    // follows the finger); wheel notches stay notches.
    [[nodiscard]] constexpr PointerEvent to_content(PointerEvent event) const noexcept {
        event.x = to_content(event.x);
        event.y = to_content(event.y);
        event.scroll_x = length(event.scroll_x);
        event.scroll_y = length(event.scroll_y);
        return event;
    }

    [[nodiscard]] constexpr DropEvent to_content(DropEvent event) const noexcept {
        event.x = to_content(event.x);
        event.y = to_content(event.y);
        return event;
    }
};

}  // namespace rummage::ui
