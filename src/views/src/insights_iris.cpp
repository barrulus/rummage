#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/views/format.hpp"
#include "rummage/views/insights_view.hpp"

namespace rummage::views {
namespace {
constexpr float turn = 2 * std::numbers::pi_v<float>;
constexpr std::size_t max_segments = 48;
constexpr std::size_t max_context = 3;

ui::Color accent_for(const InsightsView::Row& row, const theme::Palette& palette) {
    // Share the categorical colours used by the space map, with stable identities across
    // drill-down and sorting. A folder retains its colour when it moves to an outer ring.
    const std::array colors{ui::Color::rgb(0x7AA2F7), ui::Color::rgb(0x9ECE6A),
                            ui::Color::rgb(0xE0AF68), ui::Color::rgb(0xBB9AF7),
                            ui::Color::rgb(0x7DCFFF), ui::Color::rgb(0xF7768E)};
    const auto key = row.directory ? row.label : fs::insight_extension(row.label);
    std::uint32_t hash = 2166136261U;
    for (char ch : key)
        hash = (hash ^ static_cast<unsigned char>(ch)) * 16777619U;
    const auto color = colors[hash % colors.size()];
    return {std::lerp(palette.primary.r, color.r, 0.85F),
            std::lerp(palette.primary.g, color.g, 0.85F),
            std::lerp(palette.primary.b, color.b, 0.85F), 1};
}

ui::AnnularSector interpolate(ui::AnnularSector a, ui::AnnularSector b, float t) {
    return {std::lerp(a.x, b.x, t),         std::lerp(a.y, b.y, t),
            std::lerp(a.inner, b.inner, t), std::lerp(a.outer, b.outer, t),
            std::lerp(a.start, b.start, t), std::lerp(a.sweep, b.sweep, t)};
}
}  // namespace

std::vector<ui::AnnularSector> insight_ring(const std::vector<double>& weights, float x, float y,
                                            float inner, float outer) {
    std::vector<ui::AnnularSector> result(weights.size());
    long double total = 0;
    for (auto weight : weights)
        if (std::isfinite(weight) && weight > 0)
            total += weight;
    if (total <= 0 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(inner) ||
        !std::isfinite(outer) || inner < 0 || outer <= inner)
        return result;
    long double accumulated = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (!std::isfinite(weights[i]) || weights[i] <= 0)
            continue;
        const float start = static_cast<float>(accumulated / total) * turn - turn / 4;
        accumulated += weights[i];
        const float end = static_cast<float>(accumulated / total) * turn - turn / 4;
        result[i] = {x, y, inner, outer, start, end - start};
    }
    return result;
}

void InsightsView::set_iris_motion(float duration_seconds) {
    iris_duration_ = std::isfinite(duration_seconds) ? std::clamp(duration_seconds, 0.0F, 2.0F) : 0;
    if (iris_duration_ == 0)
        iris_animating_ = false;
}

void InsightsView::invalidate_iris() {
    iris_dirty_ = true;
    // Old hit targets refer to the previous row order/snapshot. Keep only the geometry for
    // animation; no stale file actions can run between a model change and the next frame.
    std::erase_if(controls_, [](const Control& control) { return control.sector.has_value(); });
}

void InsightsView::draw_iris(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette,
                             ui::IconSet& icons, const fs::MimeDatabase& mime,
                             ui::Scroller::Clock::time_point now) {
    const bool resized = iris_bounds_.x != bounds.x || iris_bounds_.y != bounds.y ||
                         iris_bounds_.width != bounds.width || iris_bounds_.height != bounds.height;
    const auto muted = theme::readable_foreground(palette.on_surface_variant, palette.surface);
    const float radius = std::min(bounds.width - 16, bounds.height - 88) / 2;
    if (radius < 24)
        return;
    const float cx = bounds.x + bounds.width / 2, cy = bounds.y + 34 + (bounds.height - 88) / 2;
    std::vector<std::size_t> ancestors;
    if (data_)
        for (auto child = scope_; child != 0 && ancestors.size() < max_context;) {
            child = data_->nodes[child].parent;
            ancestors.push_back(child);
        }
    const float context_width = std::min(15.0F, radius * 0.075F);
    const float main_outer = radius - static_cast<float>(ancestors.size()) * context_width;
    const float hole = main_outer * 0.48F;
    if (iris_dirty_ || resized || iris_snapshot_ != data_) {
        const bool same_snapshot = iris_snapshot_ == data_;
        iris_from_ = resized ? std::vector<IrisSegment>{} : iris_painted_;
        iris_target_.clear();
        iris_bounds_ = bounds;
        iris_snapshot_ = data_;
        if (data_) {
            const auto add_ring = [&](std::vector<IrisSegment> entries, std::size_t parent,
                                      std::size_t level, float inner, float outer) {
                const auto weight = [this](const IrisSegment& segment) {
                    return bytes_ ? segment.row.bytes : segment.row.files;
                };
                long double total = 0;
                for (const auto& entry : entries)
                    total += weight(entry);
                // Stable metric order keeps table sorts from spinning the diagram. Only the
                // largest entries get individual arcs; small ones share a truthful More arc.
                std::ranges::sort(entries, [&](const auto& a, const auto& b) {
                    const auto aw = weight(a), bw = weight(b);
                    return aw != bw ? aw > bw : fs::natural_compare(a.row.label, b.row.label) < 0;
                });
                IrisSegment more{.key = std::format("more:{}", parent),
                                 .row = {.directory = true},
                                 .shape = {},
                                 .index = rows_.size(),
                                 .parent = parent,
                                 .level = level,
                                 .more = true};
                std::size_t grouped = 0, shown = 0;
                std::vector<IrisSegment> ring;
                std::vector<double> weights;
                for (auto& entry : entries) {
                    const auto amount = weight(entry);
                    if (amount == 0)
                        continue;
                    if (!entry.on_path &&
                        (shown >= max_segments || static_cast<long double>(amount) < total / 360)) {
                        more.row.files += entry.row.files;
                        more.row.bytes += entry.row.bytes;
                        more.row.partial = more.row.partial || entry.row.partial;
                        more.index = std::min(more.index, entry.index);
                        ++grouped;
                        continue;
                    }
                    weights.push_back(static_cast<double>(amount));
                    ring.push_back(std::move(entry));
                    ++shown;
                }
                if (grouped != 0) {
                    more.row.label = i18n::trf("{} more", grouped);
                    weights.push_back(static_cast<double>(weight(more)));
                    ring.push_back(std::move(more));
                }
                const auto shapes = insight_ring(weights, cx, cy, inner, outer);
                for (std::size_t i = 0; i < ring.size(); ++i) {
                    ring[i].shape = shapes[i];
                    // A one-pixel separator, capped so tiny measured portions aren't erased.
                    const float gap = shapes[i].sweep >= turn - 0.0001F
                                          ? 0
                                          : std::min(1.5F / outer, shapes[i].sweep * 0.12F);
                    ring[i].shape.start += gap / 2;
                    ring[i].shape.sweep -= gap;
                    iris_target_.push_back(std::move(ring[i]));
                }
            };
            std::vector<IrisSegment> entries;
            for (std::size_t i = 0; i < rows_.size(); ++i)
                entries.push_back({.key = std::to_string(rows_[i].node),
                                   .row = rows_[i],
                                   .shape = {},
                                   .index = i,
                                   .parent = scope_});
            add_ring(std::move(entries), scope_, 0, hole + 5, main_outer);
            auto on_path = scope_;
            for (std::size_t level = 0; level < ancestors.size(); ++level) {
                const auto parent = ancestors[level];
                std::vector<IrisSegment> children;
                for (auto child = parent + 1; child < data_->nodes[parent].end;
                     child = data_->nodes[child].end) {
                    const auto& node = data_->nodes[child];
                    children.push_back({.key = std::to_string(child),
                                        .row = {.label = node.name,
                                                .node = child,
                                                .files = node.files,
                                                .bytes = node.bytes,
                                                .directory = node.directory,
                                                .partial = node.partial},
                                        .shape = {},
                                        .parent = parent,
                                        .level = level + 1,
                                        .on_path = child == on_path});
                }
                const float inner = main_outer + static_cast<float>(level) * context_width + 3;
                add_ring(std::move(children), parent, level + 1, inner,
                         main_outer + static_cast<float>(level + 1) * context_width);
                on_path = parent;
            }
        }
        // Node identities only mean the same thing inside the same scan. A new scan fades
        // in without morphing unrelated paths; navigation inside a scan preserves the rings.
        if (resized || !same_snapshot)
            iris_from_.clear();
        iris_started_ = now;
        iris_animating_ = iris_duration_ > 0 && !iris_target_.empty();
        iris_dirty_ = false;
    }
    float progress =
        iris_animating_
            ? std::clamp(std::chrono::duration<float>(now - iris_started_).count() / iris_duration_,
                         0.0F, 1.0F)
            : 1;
    if (progress >= 1)
        iris_animating_ = false;
    const float eased = ui::Motion::ease(progress);
    iris_painted_.clear();
    renderer.push_clip(bounds);
    const auto paint = [&](const IrisSegment& segment, bool interactive) {
        const auto& shape = segment.shape;
        const auto id = segment.level == 0 ? (segment.more ? std::string{"iris-more"}
                                                           : std::format("tile:{}", segment.index))
                                           : "iris-context:" + segment.key;
        const bool row_hot =
            segment.level == 0 && !segment.more &&
            (hover_ == std::format("row:{}", segment.index) ||
             focus_ == std::format("row:{}", segment.index) || selected(segment.index));
        const bool hot = interactive && (hover_ == id || focus_ == id || row_hot);
        const auto accent = segment.more ? palette.outline : accent_for(segment.row, palette);
        const float opacity = segment.level == 0 ? (hot ? 0.95F : 0.72F)
                              : segment.on_path  ? 0.85F
                              : hot              ? 0.65F
                                                 : 0.25F;
        renderer.draw_sector(shape, accent.with_alpha(opacity * segment.opacity));
        if (hot || segment.on_path) {
            auto rim = shape;
            rim.inner = std::max(shape.inner, shape.outer - 2);
            renderer.draw_sector(rim, palette.on_surface.with_alpha(segment.opacity * 0.65F));
        }
        const float mid = shape.start + shape.sweep / 2;
        const float r = (shape.inner + shape.outer) / 2;
        const float px = shape.x + r * std::cos(mid), py = shape.y + r * std::sin(mid);
        const ui::Rect target{px - 10, py - 10, 20, 20};
        if (interactive) {
            controls_.push_back(
                {id, target,
                 [this, segment] {
                     if (segment.level == 0) {
                         if (!segment.more)
                             activate_row(segment.index);
                         else {
                             requested_scroll_ = static_cast<float>(segment.index) * 44;
                             focus_ = std::format("row:{}", segment.index);
                             redraw_();
                         }
                     } else if (data_) {
                         const auto node = segment.more ? segment.parent : segment.row.node;
                         const auto destination =
                             node == 0 ? data_->root : data_->root / data_->relative_path(node);
                         if (segment.more || segment.row.directory)
                             navigate(destination);
                         else {
                             navigate(destination.parent_path());
                             const auto found = std::ranges::find(rows_, node, &Row::node);
                             if (found != rows_.end()) {
                                 const auto index = static_cast<std::size_t>(found - rows_.begin());
                                 select_row(index);
                                 requested_scroll_ = static_cast<float>(index) * 44;
                             }
                         }
                     }
                 },
                 shape});
        }
        // Files are recognisable native MIME icons, folders use the same folder icon as the
        // browser. Only draw when there is real arc space; the list labels every entry.
        if (segment.level == 0 && shape.sweep * r >= 32 && shape.outer - shape.inner >= 30) {
            const auto type = segment.row.directory ? fs::MimeDatabase::kDirectory
                                                    : mime.by_name(segment.row.label).mime;
            if (segment.more)
                icons.draw(renderer, "chevron-down", px - 10, py - 10, 20,
                           palette.on_surface.with_alpha(segment.opacity));
            else {
                const float label_width = std::min(100.0F, (shape.outer - shape.inner) * 0.8F);
                const bool labelled = label_width >= 52 && shape.sweep * r >= 100;
                icons.draw_file(renderer, mime.name(type), fs::icon_name(mime, type), px - 12,
                                py - (labelled ? 22 : 12), 24,
                                palette.on_surface.with_alpha(segment.opacity));
                if (labelled) {
                    const float width = std::min(
                        label_width,
                        renderer.fonts().layout(segment.row.label, 11, renderer.scale()).width /
                            renderer.scale());
                    renderer.draw_text(segment.row.label, px - width / 2, py + 6, 11,
                                       palette.on_surface.with_alpha(segment.opacity), width,
                                       ui::Ellipsis::middle);
                }
            }
        }
    };
    if (progress < 1)
        for (const auto& old : iris_from_) {
            if (std::ranges::find(iris_target_, old.key, &IrisSegment::key) != iris_target_.end())
                continue;
            auto fading = old;
            fading.opacity *= 1 - eased;
            fading.shape.inner *= 1 + eased * 0.15F;
            fading.shape.outer *= 1 + eased * 0.15F;
            paint(fading, false);
        }
    for (const auto& target : iris_target_) {
        auto segment = target;
        if (progress < 1) {
            auto from = target.shape;
            from.inner *= 0.8F;
            from.outer *= 0.8F;
            const auto old = std::ranges::find(iris_from_, target.key, &IrisSegment::key);
            if (old != iris_from_.end())
                from = old->shape;
            segment.shape = interpolate(from, target.shape, eased);
            segment.opacity =
                old != iris_from_.end() ? std::lerp(old->opacity, 1.0F, eased) : eased;
        }
        paint(segment, true);
        iris_painted_.push_back(std::move(segment));
    }
    // The hub always goes up; its label doesn't change under the pointer. Hover details sit
    // below the chart so the navigation target remains clear.
    const ui::AnnularSector hub{cx, cy, 0, std::max(0.0F, hole - 2), 0, turn};
    renderer.draw_sector(hub, palette.primary.with_alpha(hover_ == "iris-up" ? 0.16F : 0.065F));
    if (focus_ == "iris-up") {
        auto rim = hub;
        rim.inner = std::max(0.0F, rim.outer - 2);
        renderer.draw_sector(rim, palette.primary);
    }
    const auto current = scope_path();
    const auto name = current.filename().empty() ? current.string() : current.filename().string();
    if (hole >= 32) {
        icons.draw(renderer, can_up() ? "chevron-up" : "folder", cx - 10, cy - 29, 20, muted);
        const float text_width =
            std::min(hole * 1.6F,
                     renderer.fonts().layout(name, 13, renderer.scale()).width / renderer.scale());
        renderer.draw_text(name, cx - text_width / 2, cy, 13, palette.on_surface, text_width,
                           ui::Ellipsis::middle);
        const auto label = i18n::tr("Up");
        if (can_up())
            renderer.draw_text(label, cx - 14, cy + 20, 11, muted, 2 * hole - 12);
    } else
        icons.draw(renderer, "chevron-up", cx - 8, cy - 8, 16, muted);
    if (can_up())
        controls_.push_back({"iris-up", {cx - 12, cy - 12, 24, 24}, [this] { up(); }, hub});
    // Explicit ancestor controls explain the context rings and remain available by keyboard.
    if (!ancestors.empty()) {
        const float chip_width =
            std::min(160.0F, bounds.width / static_cast<float>(ancestors.size()));
        for (std::size_t i = 0; i < ancestors.size(); ++i) {
            const auto parent = ancestors[i];
            const auto destination =
                parent == 0 ? data_->root : data_->root / data_->relative_path(parent);
            const auto label = destination.filename().empty() ? destination.string()
                                                              : destination.filename().string();
            const ui::Rect chip{bounds.x + static_cast<float>(i) * chip_width, bounds.y,
                                chip_width - 4, 26};
            const auto id = std::format("iris-parent:{}", i);
            icons.draw(renderer, "chevron-up", chip.x + 4, chip.y + 5, 14, muted);
            renderer.draw_text(label, chip.x + 22, chip.y + 5, 12, muted, chip.width - 26,
                               ui::Ellipsis::middle);
            if (focus_ == id || hover_ == id)
                renderer.draw_box(chip, {.fill = palette.primary.with_alpha(0.06F),
                                         .border_width = 1,
                                         .border = palette.primary});
            controls_.push_back({id, chip, [this, destination] { navigate(destination); }});
        }
    }
    const IrisSegment* detail = nullptr;
    for (const auto& segment : iris_target_) {
        const auto id = segment.level == 0 ? (segment.more ? std::string{"iris-more"}
                                                           : std::format("tile:{}", segment.index))
                                           : "iris-context:" + segment.key;
        if (hover_ == id || focus_ == id ||
            (segment.level == 0 && !segment.more &&
             hover_ == std::format("row:{}", segment.index))) {
            detail = &segment;
            break;
        }
    }
    if (!detail)
        for (const auto& segment : iris_target_)
            if (segment.level == 0 && !segment.more && selected(segment.index)) {
                detail = &segment;
                break;
            }
    const float caption_y = bounds.bottom() - 44;
    if (detail) {
        auto amount = bytes_ ? format_size(detail->row.bytes)
                             : i18n::trn("{} file", "{} files", detail->row.files);
        if (detail->row.partial)
            amount = i18n::trf("≥ {}", amount);
        renderer.draw_text(detail->row.label, bounds.x + 8, caption_y, 13, palette.on_surface,
                           bounds.width - 16, ui::Ellipsis::middle);
        renderer.draw_text(amount, bounds.x + 8, caption_y + 22, 12, muted, bounds.width - 16);
    } else {
        renderer.draw_text(iris_target_.empty()
                               ? (loading_ ? i18n::tr("No files measured yet")
                                           : i18n::tr("No area for zero values"))
                               : i18n::tr("Inner ring: contents · Outer rings: parents"),
                           bounds.x + 8, caption_y, 12, muted, bounds.width - 16);
        renderer.draw_text(i18n::tr("Select a segment to explore · Centre goes up"), bounds.x + 8,
                           caption_y + 22, 12, muted, bounds.width - 16);
    }
    renderer.pop_clip();
}
}  // namespace rummage::views
