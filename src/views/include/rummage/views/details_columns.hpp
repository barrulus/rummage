#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "rummage/core/i18n.hpp"
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
    DetailsColumn{fs::SortKey::name, "name", i18n::mark("Name"), 0.0F, 160.0F},
    DetailsColumn{fs::SortKey::size, "size", i18n::mark("Size"), 96.0F, 64.0F},
    DetailsColumn{fs::SortKey::modified, "modified", i18n::mark("Modified"), 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::type, "type", i18n::mark("Type"), 220.0F, 80.0F},
    DetailsColumn{fs::SortKey::created, "created", i18n::mark("Created"), 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::accessed, "accessed", i18n::mark("Accessed"), 150.0F, 96.0F},
    DetailsColumn{fs::SortKey::owner, "owner", i18n::mark("Owner"), 110.0F, 80.0F},
    DetailsColumn{fs::SortKey::group, "group", i18n::mark("Group"), 110.0F, 80.0F},
    DetailsColumn{fs::SortKey::permissions, "permissions", i18n::mark("Permissions"), 130.0F,
                  110.0F},
    DetailsColumn{fs::SortKey::location, "location", i18n::mark("Location"), 220.0F, 100.0F},
    DetailsColumn{fs::SortKey::link_target, "link_target", i18n::mark("Link target"), 220.0F,
                  100.0F},
    DetailsColumn{fs::SortKey::dimensions, "dimensions", i18n::mark("Dimensions"), 140.0F, 90.0F},
    DetailsColumn{fs::SortKey::duration, "duration", i18n::mark("Duration"), 130.0F, 90.0F},
    DetailsColumn{fs::SortKey::bitrate, "bitrate", i18n::mark("Bitrate"), 130.0F, 90.0F},
    DetailsColumn{fs::SortKey::video_codec, "video_codec", i18n::mark("Video codec"), 130.0F,
                  90.0F},
    DetailsColumn{fs::SortKey::audio_codec, "audio_codec", i18n::mark("Audio codec"), 130.0F,
                  90.0F}};
inline constexpr std::size_t kDetailsColumnCount = kDetailsColumns.size();
// Existing property widths keep their indices; the last slot is Name (0 = automatic).
using DetailsWidths = std::array<float, kDetailsColumnCount>;
inline constexpr float kMaxDetailsWidth = 4096.0F;
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
