#include <algorithm>
#include <array>
#include <format>
#include <numeric>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/views/format.hpp"
#include "rummage/views/insights_view.hpp"

namespace rummage::views {
InsightCategory insight_category(std::string_view name, bool directory,
                                 const fs::MimeDatabase& mime) {
    if (directory)
        return InsightCategory::folders;
    const auto extension = fs::insight_extension(std::string{name});
    const auto in = [&](std::string_view extensions) {
        return extensions.find("|" + extension + "|") != std::string_view::npos;
    };
    if (in("|.c|.h|.cpp|.hpp|.cc|.rs|.py|.js|.jsx|.ts|.tsx|.go|.java|.kt|.swift|.rb|.sh|.bash|.nix|"
           ".lua|.css|.html|.json|.toml|.yaml|.yml|.xml|.sql|.cmake|"))
        return InsightCategory::code;
    const auto type = mime.by_name(name).mime;
    const auto media = mime.name(type);
    if (media.starts_with("image/"))
        return InsightCategory::images;
    if (media.starts_with("audio/"))
        return InsightCategory::audio;
    if (media.starts_with("video/"))
        return InsightCategory::video;
    if (in("|.zip|.tar|.tar.gz|.tar.xz|.tar.bz2|.tar.zst|.gz|.bz2|.xz|.zst|.7z|.rar|.tgz|.deb|."
           "rpm|") ||
        mime.generic_icon(type) == "package-x-generic")
        return InsightCategory::archives;
    if (media.starts_with("text/") ||
        in("|.pdf|.doc|.docx|.odt|.rtf|.xls|.xlsx|.ods|.ppt|.pptx|.odp|.epub|.md|.txt|"))
        return InsightCategory::documents;
    return InsightCategory::other;
}

ui::Color insight_color(InsightCategory category) {
    switch (category) {
        case InsightCategory::folders:
            return ui::Color::rgb(0x7895AC);
        case InsightCategory::documents:
            return ui::Color::rgb(0x5299ED);
        case InsightCategory::code:
            return ui::Color::rgb(0x45BA8A);
        case InsightCategory::images:
            return ui::Color::rgb(0xBA7FE3);
        case InsightCategory::audio:
            return ui::Color::rgb(0xE5B24C);
        case InsightCategory::video:
            return ui::Color::rgb(0xEA7C8B);
        case InsightCategory::archives:
            return ui::Color::rgb(0xE79550);
        case InsightCategory::other:
            return ui::Color::rgb(0x9099A8);
    }
    return {};
}

ui::Color InsightsView::row_color(const Row& row, const theme::Palette&,
                                  const fs::MimeDatabase& mime) const {
    if (view_ == View::age) {
        const std::array colors{0x4DB6ACU, 0x78B96EU, 0xB6B34DU, 0xD69D55U, 0xCE7781U, 0x9A86CEU};
        const auto age = static_cast<std::size_t>(std::stoul(row.extension));
        return ui::Color::rgb(colors[std::min(age, colors.size() - 1)]);
    }
    if (row.other)
        return insight_color(InsightCategory::other);
    const auto name =
        view_ == View::types || view_ == View::other_types ? "file" + row.extension : row.label;
    return insight_color(insight_category(name, row.directory, mime));
}

void InsightsView::draw_legend(ui::Renderer& renderer, ui::Rect bounds,
                               const theme::Palette& palette, const fs::MimeDatabase&) const {
    constexpr std::array present{InsightCategory::folders,  InsightCategory::documents,
                                 InsightCategory::code,     InsightCategory::images,
                                 InsightCategory::audio,    InsightCategory::video,
                                 InsightCategory::archives, InsightCategory::other};
    const std::array labels{i18n::tr("Folders"),  i18n::tr("Documents"), i18n::tr("Code"),
                            i18n::tr("Images"),   i18n::tr("Audio"),     i18n::tr("Video"),
                            i18n::tr("Archives"), i18n::tr("Other")};
    float x = bounds.x, y = bounds.y;
    renderer.push_clip(bounds);
    for (const auto category : present) {
        const auto label = labels[static_cast<std::size_t>(category)];
        const float width =
            renderer.fonts().layout(label, 11, renderer.scale()).width / renderer.scale() + 24;
        if (x > bounds.x && x + width > bounds.right()) {
            x = bounds.x;
            y += 20;
        }
        renderer.fill_rect({x, y + 4, 8, 8}, insight_color(category));
        renderer.draw_text(label, x + 13, y, 11, palette.on_surface_variant,
                           std::max(1.0F, bounds.right() - x - 13));
        x += width;
    }
    renderer.pop_clip();
}

void InsightsView::draw_map(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette,
                            ui::IconSet& icons, const fs::MimeDatabase& mime) {
    const auto muted = theme::readable_foreground(palette.on_surface_variant, palette.surface);
    const ui::Rect map{bounds.x, bounds.y, bounds.width, std::max(0.0F, bounds.height - 90)};
    if (map_layout_.dirty || map_layout_.bounds.width != map.width ||
        map_layout_.bounds.height != map.height) {
        map_layout_ = {};
        map_layout_.bounds = map;
        map_layout_.first = rows_.size();
        map_layout_.more.other = true;
        std::vector<std::size_t> order(rows_.size());
        std::iota(order.begin(), order.end(), 0);
        const auto value = [this](std::size_t i) {
            return bytes_ ? rows_[i].bytes : rows_[i].files;
        };
        std::ranges::sort(order, [&](auto a, auto b) {
            return value(a) != value(b) ? value(a) > value(b)
                                        : fs::natural_compare(rows_[a].label, rows_[b].label) < 0;
        });
        long double total = 0;
        for (const auto i : order)
            total += value(i);
        auto& shown = map_layout_.shown;
        auto& weights = map_layout_.weights;
        auto& grouped = map_layout_.grouped;
        auto& first = map_layout_.first;
        auto& more = map_layout_.more;
        for (const auto i : order) {
            if (value(i) == 0)
                continue;
            const auto area =
                total > 0 ? static_cast<long double>(value(i)) / total * map.width * map.height : 0;
            if (shown.size() >= 48 || area < 500) {
                ++grouped;
                first = std::min(first, i);
                more.files += rows_[i].files;
                more.bytes += rows_[i].bytes;
                more.partial = more.partial || rows_[i].partial;
            } else {
                shown.push_back(i);
                weights.push_back(static_cast<double>(value(i)));
            }
        }
        if (grouped) {
            more.label = i18n::trf("{} more", grouped);
            weights.push_back(static_cast<double>(bytes_ ? more.bytes : more.files));
        }
        map_layout_.dirty = false;
    }
    const auto& shown = map_layout_.shown;
    const auto& weights = map_layout_.weights;
    const auto& more = map_layout_.more;
    const auto first = map_layout_.first;
    const auto tiles = insight_treemap(weights, map);
    std::string detail;
    renderer.push_clip(map);
    for (std::size_t k = 0; k < tiles.size(); ++k) {
        const bool grouped_tile = k == shown.size();
        const auto i = grouped_tile ? first : shown[k];
        const auto& row = grouped_tile ? more : rows_[i];
        const auto rect =
            tiles[k].inset(std::min(2.0F, std::min(tiles[k].width, tiles[k].height) / 8));
        const auto id = grouped_tile ? std::string{"map-more"} : std::format("tile:{}", i);
        const bool hot = hover_ == id || focus_ == id ||
                         (!grouped_tile && (hover_ == std::format("row:{}", i) ||
                                            focus_ == std::format("row:{}", i) || selected(i)));
        const auto accent = row_color(row, palette, mime);
        renderer.draw_box(
            rect, {.fill = accent.with_alpha(hot ? 0.34F : 0.20F),
                   .radius = ui::Corners::all(std::min({6.0F, rect.width / 6, rect.height / 6})),
                   .border_width = hot ? 2.0F : 0,
                   .border = accent});
        auto amount = bytes_ ? format_size(row.bytes) : i18n::trn("{} file", "{} files", row.files);
        if (row.partial)
            amount = i18n::trf("≥ {}", amount);
        const auto label = grouped_tile ? std::format("{} · {}", row.label, amount) : row.label;
        const float label_width =
            renderer.fonts().layout(label, 13, renderer.scale()).width / renderer.scale();
        const bool labelled = rect.height >= 66 && label_width + 24 <= rect.width;
        if (rect.width >= 24 && rect.height >= 24) {
            const float size = std::min({row.directory || grouped_tile ? 28.0F : 64.0F,
                                         rect.width - 12, rect.height - (labelled ? 44 : 8)});
            const float ix = rect.x + (rect.width - size) / 2;
            const float iy = rect.y + (rect.height - size - (labelled ? 38 : 0)) / 2;
            if (grouped_tile)
                icons.draw(renderer, "chevron-down", ix, iy, size, accent);
            else {
                const auto type =
                    row.directory ? fs::MimeDatabase::kDirectory : mime.by_name(row.label).mime;
                icons.draw_file(renderer, mime.name(type), fs::icon_name(mime, type), ix, iy, size,
                                accent);
            }
            if (labelled) {
                renderer.draw_text(label, rect.x + (rect.width - label_width) / 2, iy + size + 6,
                                   13, palette.on_surface, label_width);
                if (!grouped_tile) {
                    const float aw = renderer.fonts().layout(amount, 12, renderer.scale()).width /
                                     renderer.scale();
                    if (aw + 16 <= rect.width)
                        renderer.draw_text(amount, rect.x + (rect.width - aw) / 2, iy + size + 25,
                                           12, muted, aw);
                }
            }
        }
        if (hot)
            detail = std::format("{} · {}", row.label, amount);
        controls_.push_back({id, rect, [this, i, grouped_tile] {
                                 if (!grouped_tile)
                                     activate_row(i);
                                 else {
                                     requested_scroll_ = static_cast<float>(i) * 44;
                                     focus_ = std::format("row:{}", i);
                                     redraw_();
                                 }
                             }});
    }
    renderer.pop_clip();
    if (tiles.empty())
        detail = loading_ ? i18n::tr("No files measured yet") : i18n::tr("No area for zero values");
    renderer.draw_text(detail, bounds.x + 4, map.bottom() + 8, 12, palette.on_surface,
                       bounds.width - 8, ui::Ellipsis::middle);
    draw_legend(renderer, {bounds.x + 4, map.bottom() + 36, bounds.width - 8, 48}, palette, mime);
}
}  // namespace rummage::views
