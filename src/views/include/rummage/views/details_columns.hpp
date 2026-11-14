#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "rummage/fs/sort.hpp"

namespace rummage::views {

struct DetailsColumn {
    fs::SortKey field;
    std::string_view id;
    std::string_view title;
    float width;
    float minimum;
};

inline constexpr std::array kDetailsColumns{
    DetailsColumn{fs::SortKey::name, "name", "Name", 0.0F, 160.0F},
    DetailsColumn{fs::SortKey::size, "size", "Size", 96.0F, 64.0F},
    DetailsColumn{fs::SortKey::modified, "modified", "Modified", 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::type, "type", "Type", 220.0F, 80.0F},
    DetailsColumn{fs::SortKey::created, "created", "Created", 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::accessed, "accessed", "Accessed", 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::owner, "owner", "Owner", 110.0F, 80.0F},
    DetailsColumn{fs::SortKey::group, "group", "Group", 110.0F, 80.0F},
    DetailsColumn{fs::SortKey::permissions, "permissions", "Permissions", 130.0F, 110.0F},
    DetailsColumn{fs::SortKey::location, "location", "Location", 220.0F, 100.0F},
    DetailsColumn{fs::SortKey::link_target, "link_target", "Link target", 220.0F, 100.0F}};
inline constexpr std::size_t kDetailsColumnCount = kDetailsColumns.size();
using DetailsWidths = std::array<float, kDetailsColumnCount - 1>;
inline constexpr DetailsWidths kDetailsWidths = [] {
    DetailsWidths widths{};
    for (std::size_t i = 1; i < kDetailsColumns.size(); ++i)
        widths[i - 1] = kDetailsColumns[i].width;
    return widths;
}();
static_assert([] {
    for (std::size_t i = 0; i < kDetailsColumns.size(); ++i)
        if (static_cast<std::size_t>(kDetailsColumns[i].field) != i)
            return false;
    return true;
}());
inline const std::vector<fs::SortKey> kDefaultDetailsColumns{
    fs::SortKey::name, fs::SortKey::size, fs::SortKey::modified, fs::SortKey::type};

}  // namespace rummage::views
