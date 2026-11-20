#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "rummage/picture/load.hpp"
#include "rummage/ui/color.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/texture.hpp"

namespace rummage::views {

// The visual actually painted for an entry, retained across an asynchronous file operation.
// The thumbnail cache keeps ownership of pinned textures and evicts only during frame cleanup.
struct ItemVisual {
    ui::Rect bounds{}, clip{};
    std::filesystem::path path;
    std::shared_ptr<const ui::Texture> thumbnail;
    std::string mime, icon, folder_icon;
    ui::Color color{}, run_color{}, run_background{};
    picture::Failure failure{picture::Failure::none};
    float opacity{1}, rail{0};
    bool executable{false};
};

// Bounds of painted items, clipped to the viewport. Reset before each layout draw.
struct ItemBounds {
    ui::Rect cursor{}, selection{};
    ItemVisual visual;

    void include(ui::Rect item, ui::Rect viewport, bool focused, bool selected) {
        const auto visible = item.intersect(viewport);
        if (focused)
            cursor = visible;
        if (selected)
            selection = selection.united(visible);
    }
};

}  // namespace rummage::views
