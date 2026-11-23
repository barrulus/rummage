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

std::vector<InsightSlice> insight_sunburst(const fs::InsightSnapshot& snapshot,
                                           const std::vector<std::size_t>& roots, bool bytes) {
    struct Branch {
        std::vector<std::size_t> children;
        std::size_t parent, depth;
        float start, sweep;
    };

    std::vector<InsightSlice> result;
    if (roots.empty())
        return result;
    std::vector<Branch> pending{{roots, snapshot.nodes[roots.front()].parent, 0, -turn / 4, turn}};
    for (std::size_t at = 0; at < pending.size() && result.size() < 256; ++at) {
        auto branch = std::move(pending[at]);
        const auto weight = [&](std::size_t node) {
            return bytes ? snapshot.nodes[node].bytes : snapshot.nodes[node].files;
        };
        std::ranges::sort(branch.children, [&](auto a, auto b) {
            return weight(a) != weight(b)
                       ? weight(a) > weight(b)
                       : fs::natural_compare(snapshot.nodes[a].name, snapshot.nodes[b].name) < 0;
        });
        long double total = 0;
        for (auto node : branch.children)
            total += weight(node);
        if (total <= 0)
            continue;
        const auto limit = std::min<std::size_t>(48, 255 - result.size());
        std::vector<InsightSlice> ring;
        InsightSlice more{.node = branch.parent, .parent = branch.parent, .depth = branch.depth};
        for (auto node : branch.children) {
            if (weight(node) == 0)
                continue;
            const auto& entry = snapshot.nodes[node];
            const auto angle =
                static_cast<float>(static_cast<long double>(weight(node)) / total) * branch.sweep;
            if (ring.size() >= limit || angle < turn / 360) {
                if (more.grouped == 0)
                    more.first = node;
                ++more.grouped;
                more.files += entry.files;
                more.bytes += entry.bytes;
                more.partial = more.partial || entry.partial;
            } else
                ring.push_back({.node = node,
                                .parent = branch.parent,
                                .depth = branch.depth,
                                .files = entry.files,
                                .bytes = entry.bytes,
                                .partial = entry.partial});
        }
        if (more.grouped)
            ring.push_back(more);
        long double accumulated = 0;
        for (auto slice : ring) {
            slice.start = branch.start + static_cast<float>(accumulated / total) * branch.sweep;
            accumulated += bytes ? slice.bytes : slice.files;
            const float end = branch.start + static_cast<float>(accumulated / total) * branch.sweep;
            slice.sweep = end - slice.start;
            result.push_back(slice);
            if (slice.grouped || slice.depth >= 2 || !snapshot.nodes[slice.node].directory)
                continue;
            std::vector<std::size_t> children;
            for (auto child = slice.node + 1; child < snapshot.nodes[slice.node].end;
                 child = snapshot.nodes[child].end)
                children.push_back(child);
            if (!children.empty())
                pending.push_back(
                    {std::move(children), slice.node, slice.depth + 1, slice.start, slice.sweep});
        }
    }
    return result;
}

bool InsightsView::chart_hover(std::size_t index) const {
    if (hover_ == std::format("tile:{}", index))
        return true;
    if (view_ != View::iris)
        return false;
    return std::ranges::any_of(iris_target_, [&](const IrisSegment& segment) {
        return segment.level > 0 && segment.index == index && hover_ == "iris-node:" + segment.key;
    });
}

void InsightsView::set_iris_motion(float duration_seconds) {
    iris_duration_ = std::isfinite(duration_seconds) ? std::clamp(duration_seconds, 0.0F, 2.0F) : 0;
    if (iris_duration_ == 0)
        iris_animating_ = false;
}

void InsightsView::invalidate_iris() {
    iris_dirty_ = true;
    map_layout_.dirty = true;
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
    const float radius = std::min(bounds.width - 16, bounds.height - 126) / 2;
    if (radius < 24)
        return;
    const float cx = bounds.x + bounds.width / 2, cy = bounds.y + 4 + (bounds.height - 126) / 2;
    const float hole = radius * 0.28F;
    if (iris_dirty_ || resized || iris_snapshot_ != data_) {
        const bool same_snapshot = iris_snapshot_ == data_;
        iris_from_ = resized ? std::vector<IrisSegment>{} : iris_painted_;
        iris_target_.clear();
        iris_bounds_ = bounds;
        iris_snapshot_ = data_;
        if (data_) {
            std::vector<std::size_t> roots;
            for (const auto& row : rows_)
                roots.push_back(row.node);
            const auto slices = insight_sunburst(*data_, roots, bytes_);
            std::size_t levels = 1;
            for (const auto& slice : slices)
                levels = std::max(levels, slice.depth + 1);
            const float band = (radius - hole - 5) / static_cast<float>(levels);
            for (const auto& slice : slices) {
                const auto& node = data_->nodes[slice.node];
                const auto source = slice.grouped ? slice.first : slice.node;
                // Link each deeper segment to its immediate ancestor's table row.
                auto ancestor = source;
                while (data_->nodes[ancestor].parent != scope_ && ancestor != 0)
                    ancestor = data_->nodes[ancestor].parent;
                const auto found = std::ranges::find(rows_, ancestor, &Row::node);
                const auto index = static_cast<std::size_t>(found - rows_.begin());
                Row row{.label = slice.grouped ? i18n::trf("{} more", slice.grouped) : node.name,
                        .node = source,
                        .files = slice.files,
                        .bytes = slice.bytes,
                        .directory = slice.grouped || node.directory,
                        .partial = slice.partial,
                        .other = slice.grouped != 0};
                if (slice.depth == 0 && !slice.grouped && found != rows_.end())
                    row = *found;
                const float inner = hole + 5 + static_cast<float>(slice.depth) * band;
                const float outer = inner + band - 3;
                const float gap =
                    slice.sweep >= turn - 0.0001F ? 0 : std::min(1.2F / outer, slice.sweep * 0.12F);
                iris_target_.push_back(
                    {.key = slice.grouped ? std::format("more:{}", slice.parent)
                                          : std::to_string(slice.node),
                     .row = std::move(row),
                     .shape = {cx, cy, inner, outer, slice.start + gap / 2, slice.sweep - gap},
                     .index = index,
                     .parent = slice.parent,
                     .level = slice.depth,
                     .more = slice.grouped != 0});
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
                                           : "iris-node:" + segment.key;
        const bool row_hot = !segment.more && (hover_ == std::format("row:{}", segment.index) ||
                                               focus_ == std::format("row:{}", segment.index) ||
                                               selected(segment.index));
        const bool hot = interactive && (hover_ == id || focus_ == id || row_hot);
        const auto accent = row_color(segment.row, palette, mime);
        const float opacity = hot ? 0.95F : 0.76F;
        renderer.draw_sector(shape, accent.with_alpha(opacity * segment.opacity));
        if (hot) {
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
                         if (segment.more || segment.row.directory) {
                             navigate(destination);
                             if (segment.more) {
                                 const auto found =
                                     std::ranges::find(rows_, segment.row.node, &Row::node);
                                 if (found != rows_.end()) {
                                     const auto index =
                                         static_cast<std::size_t>(found - rows_.begin());
                                     requested_scroll_ = static_cast<float>(index) * 44;
                                     focus_ = std::format("row:{}", index);
                                 }
                             }
                         } else {
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
        if (shape.sweep * r >= 32 && shape.outer - shape.inner >= 30) {
            const auto type = segment.row.directory ? fs::MimeDatabase::kDirectory
                                                    : mime.by_name(segment.row.label).mime;
            if (segment.more)
                icons.draw(renderer, "chevron-down", px - 10, py - 10, 20,
                           palette.on_surface.with_alpha(segment.opacity));
            else {
                const float label_width = std::min(100.0F, (shape.outer - shape.inner) * 0.8F);
                const float name_width =
                    renderer.fonts().layout(segment.row.label, 11, renderer.scale()).width /
                    renderer.scale();
                const bool labelled = label_width >= name_width && shape.sweep * r >= 100;
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
    const IrisSegment* detail = nullptr;
    for (const auto& segment : iris_target_) {
        const auto id = segment.level == 0 ? (segment.more ? std::string{"iris-more"}
                                                           : std::format("tile:{}", segment.index))
                                           : "iris-node:" + segment.key;
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
    const float caption_y = bounds.bottom() - 112;
    if (detail) {
        auto amount = bytes_ ? format_size(detail->row.bytes)
                             : i18n::trn("{} file", "{} files", detail->row.files);
        if (detail->row.partial)
            amount = i18n::trf("≥ {}", amount);
        renderer.draw_text(detail->row.label, bounds.x + 8, caption_y, 13, palette.on_surface,
                           bounds.width - 16, ui::Ellipsis::middle);
        renderer.draw_text(amount, bounds.x + 8, caption_y + 22, 12, muted, bounds.width - 16);
    } else {
        renderer.draw_text(iris_target_.empty() ? (loading_ ? i18n::tr("No files measured yet")
                                                            : i18n::tr("No area for zero values"))
                                                : i18n::tr("Outer rings show deeper folders"),
                           bounds.x + 8, caption_y, 12, muted, bounds.width - 16);
        renderer.draw_text(i18n::tr("Select a segment to explore · Centre goes up"), bounds.x + 8,
                           caption_y + 22, 12, muted, bounds.width - 16);
    }
    draw_legend(renderer, {bounds.x + 8, bounds.bottom() - 58, bounds.width - 16, 48}, palette,
                mime);
    renderer.pop_clip();
}
}  // namespace rummage::views
