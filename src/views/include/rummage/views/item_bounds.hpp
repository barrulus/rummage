#pragma once

#include "rummage/ui/geometry.hpp"

namespace rummage::views {

// Bounds of painted items, clipped to the viewport. Reset before each layout draw.
struct ItemBounds {
    ui::Rect cursor{}, selection{};

    void include(ui::Rect item, ui::Rect viewport, bool focused, bool selected) {
        const auto visible = item.intersect(viewport);
        if (focused)
            cursor = visible;
        if (selected)
            selection = selection.united(visible);
    }
};

}  // namespace rummage::views
