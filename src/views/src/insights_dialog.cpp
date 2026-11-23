#include "rummage/views/insights_dialog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <numeric>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/views/dialog_chrome.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {
std::string extension_label(const std::string& extension) {
    return extension.empty() ? std::string{i18n::tr("No extension")} : extension;
}

ui::Color mix(ui::Color a, ui::Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}

ui::Color row_color(const InsightsDialog::Row& row, const theme::Palette& palette) {
    // Categorical chart accents stay distinct even with a monochrome desktop palette.
    // Mix in the user's accent and derive readable foregrounds on the actual surfaces.
    const std::array colors{ui::Color::rgb(0x7AA2F7), ui::Color::rgb(0x9ECE6A),
                            ui::Color::rgb(0xE0AF68), ui::Color::rgb(0xBB9AF7),
                            ui::Color::rgb(0x7DCFFF), ui::Color::rgb(0xF7768E)};
    const auto key = row.directory           ? row.label
                     : row.extension.empty() ? fs::insight_extension(row.label)
                                             : row.extension;
    std::uint32_t hash = 2166136261U;
    for (const char ch : key)
        hash = (hash ^ static_cast<unsigned char>(ch)) * 16777619U;
    return mix(palette.primary, colors[hash % colors.size()], 0.85F);
}
}  // namespace

std::vector<ui::Rect> insight_treemap(const std::vector<double>& weights, ui::Rect bounds) {
    std::vector<ui::Rect> result(weights.size());
    std::vector<std::size_t> ids;
    for (std::size_t i = 0; i < weights.size(); ++i)
        if (std::isfinite(weights[i]) && weights[i] > 0)
            ids.push_back(i);
    if (bounds.empty() || !std::isfinite(bounds.width) || !std::isfinite(bounds.height))
        return result;
    const auto divide = [&](auto&& self, std::size_t first, std::size_t end,
                            ui::Rect rect) -> void {
        if (first == end)
            return;
        if (end - first == 1) {
            result[ids[first]] = rect;
            return;
        }
        long double total = 0;
        for (auto i = first; i < end; ++i)
            total += weights[ids[i]];
        auto middle = first + 1;
        long double sum = weights[ids[first]];
        while (middle + 1 < end && sum + weights[ids[middle]] / 2 < total / 2)
            sum += weights[ids[middle++]];
        const float share = static_cast<float>(sum / total);
        auto other = rect;
        if (rect.width >= rect.height) {
            rect.width *= share;
            other.x += rect.width;
            other.width -= rect.width;
        } else {
            rect.height *= share;
            other.y += rect.height;
            other.height -= rect.height;
        }
        self(self, first, middle, rect);
        self(self, middle, end, other);
    };
    divide(divide, 0, ids.size(), bounds);
    return result;
}

InsightsDialog::InsightsDialog(core::WorkerPool& pool, std::function<void()> redraw,
                               fs::InsightOptions options)
    : pool_{pool}, redraw_{std::move(redraw)}, options_{options} {}

InsightsDialog::~InsightsDialog() {
    stop_.cancel();
    job_.cancel();
}

void InsightsDialog::open(std::filesystem::path root, bool hidden, Reveal reveal_handler) {
    const bool resume = root == entry_root_;
    const auto destination = resume ? scope_path() : root;
    entry_root_ = std::move(root);
    reveal_ = std::move(reveal_handler);
    if (!resume) {
        options_.hidden = hidden;
        history_.clear();
        extension_.clear();
        offset_ = 0;
        if (view_ == View::matches)
            view_ = View::types;
    }
    root_ = destination;
    data_.reset();
    scope_ = 0;
    focus_ = can_back() ? "back" : view_ == View::space ? "space" : "types";
    hover_.clear();
    open_ = true;
    refresh();
}

void InsightsDialog::close() {
    if (!open_)
        return;
    root_ = scope_path();
    if (loading_)
        offset_ = pending_offset_;
    stop_.cancel();
    job_.cancel();
    open_ = loading_ = false;
    scope_ = 0;
    data_.reset();
    rows_.clear();
    controls_.clear();
    hover_.clear();
    redraw_();
}

std::filesystem::path InsightsDialog::scope_path() const {
    return data_ ? (scope_ == 0 ? data_->root : data_->root / data_->relative_path(scope_)) : root_;
}

void InsightsDialog::refresh() {
    if (!open_)
        return;
    root_ = scope_path();
    stop_.cancel();
    job_.cancel();
    stop_ = core::CancelToken{};
    data_.reset();
    progress_ = {};
    pending_offset_ = offset_;
    scope_ = offset_ = 0;
    rows_.clear();
    controls_.clear();
    loading_ = true;
    job_ = pool_.submit_streaming(
        core::Priority::nearby,
        [root = root_, options = options_, stop = stop_](const core::CancelToken&, auto emit) {
            return std::make_shared<const fs::InsightSnapshot>(
                fs::scan_insights(root, options, stop, emit));
        },
        [this](fs::InsightProgress progress) {
            progress_ = progress;
            redraw_();
        },
        [this](std::shared_ptr<const fs::InsightSnapshot> data) {
            data_ = std::move(data);
            loading_ = false;
            const auto offset = pending_offset_;
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(offset, rows_.size() - 1);
        });
    redraw_();
}

void InsightsDialog::stop() {
    stop_.cancel();  // Keep delivery enabled: publish the partial snapshot when the walk stops.
    redraw_();
}

void InsightsDialog::set_view(View view) {
    if (view == view_)
        return;
    remember();
    view_ = view;
    rebuild();
}

void InsightsDialog::set_size_measure(bool bytes) {
    bytes_ = bytes;
    rebuild();
}

void InsightsDialog::set_recursive(bool recursive) {
    if (options_.recursive != recursive) {
        options_.recursive = recursive;
        refresh();
    }
}

void InsightsDialog::set_hidden(bool hidden) {
    if (options_.hidden != hidden) {
        options_.hidden = hidden;
        refresh();
    }
}

void InsightsDialog::rebuild() {
    pending_offset_ = 0;
    hover_.clear();
    rows_.clear();
    controls_.clear();
    offset_ = 0;
    if (data_ && scope_ < data_->nodes.size()) {
        std::map<std::string, Row> groups;
        for (std::size_t i = scope_ + 1; i < data_->nodes[scope_].end; ++i) {
            const auto& node = data_->nodes[i];
            if (view_ == View::space) {
                if (node.parent == scope_)
                    rows_.push_back({.label = node.name,
                                     .extension = {},
                                     .node = i,
                                     .files = node.files,
                                     .bytes = node.bytes,
                                     .directory = node.directory,
                                     .partial = node.partial});
                continue;
            }
            if (node.directory)
                continue;
            const auto extension = fs::insight_extension(node.name);
            if (view_ == View::matches) {
                if (extension == extension_)
                    // Keep each basename once; repeating a deep prefix for every match can
                    // multiply the scan's bounded name storage by the directory depth.
                    rows_.push_back({.label = node.name,
                                     .extension = extension,
                                     .node = i,
                                     .files = 1,
                                     .bytes = node.bytes});
            } else {
                auto& row = groups[extension];
                row.label = extension_label(extension);
                row.extension = extension;
                ++row.files;
                row.bytes += node.bytes;
            }
        }
        for (auto& [extension, row] : groups)
            rows_.push_back(std::move(row));
        std::ranges::sort(rows_, [this](const Row& a, const Row& b) {
            const auto left = bytes_ ? a.bytes : a.files;
            const auto right = bytes_ ? b.bytes : b.files;
            return left != right ? left > right : a.label < b.label;
        });
    }
    redraw_();
}

void InsightsDialog::reveal(std::size_t node) {
    if (!data_ || node >= data_->nodes.size())
        return;
    const auto path = data_->root / data_->relative_path(node);
    const bool directory = data_->nodes[node].directory;
    const auto handler = reveal_;
    close();
    if (handler)
        handler(path, directory);
}

void InsightsDialog::activate_row(std::size_t index) {
    if (index >= rows_.size() || !data_)
        return;
    const auto row = rows_[index];
    if (view_ == View::types) {
        remember();
        extension_ = row.extension;
        view_ = View::matches;
        rebuild();
    } else if (row.directory) {
        navigate(data_->root / data_->relative_path(row.node));
    } else {
        reveal(row.node);
    }
}

void InsightsDialog::remember() {
    if (history_.size() == 128)
        history_.erase(history_.begin());
    history_.push_back({scope_path(), view_, extension_, offset_});
}

void InsightsDialog::restore(Location location) {
    view_ = location.view;
    extension_ = std::move(location.extension);
    // Reuse complete folders in the current snapshot, including after breadcrumb jumps.
    if (data_) {
        const auto relative = location.path.lexically_relative(data_->root);
        std::size_t id = 0;
        bool found = !relative.empty();
        for (const auto& component : relative) {
            if (component == ".")
                continue;
            auto child = id + 1;
            while (child < data_->nodes[id].end &&
                   (!data_->nodes[child].directory || data_->nodes[child].name != component))
                child = data_->nodes[child].end;
            if (child >= data_->nodes[id].end) {
                found = false;
                break;
            }
            id = child;
        }
        if (found && (id == scope_ || (options_.recursive && !data_->nodes[id].partial))) {
            scope_ = id;
            rebuild();
            offset_ = rows_.empty() ? 0 : std::min(location.offset, rows_.size() - 1);
            focus_ = "back";
            return;
        }
    }
    root_ = std::move(location.path);
    data_.reset();
    scope_ = 0;
    offset_ = location.offset;
    focus_ = "back";
    refresh();
}

void InsightsDialog::navigate(const std::filesystem::path& path) {
    if (path.empty() || path == scope_path())
        return;
    remember();
    restore({path, view_ == View::matches ? View::types : view_, {}, 0});
}

void InsightsDialog::back() {
    if (history_.empty())
        return;
    auto previous = std::move(history_.back());
    history_.pop_back();
    restore(std::move(previous));
}

bool InsightsDialog::can_up() const {
    const auto path = scope_path();
    return !path.empty() && path.parent_path() != path;
}

void InsightsDialog::up() {
    if (can_up())
        navigate(scope_path().parent_path());
}

void InsightsDialog::page(int direction) {
    if (direction > 0 && offset_ + page_size_ < rows_.size())
        offset_ += page_size_;
    else if (direction < 0)
        offset_ = offset_ > page_size_ ? offset_ - page_size_ : 0;
    controls_.clear();
    redraw_();
}

std::string InsightsDialog::status() const {
    if (loading_)
        return stop_.cancelled()
                   ? std::string{i18n::tr("Stopping…")}
                   : i18n::trf("Counting… {}", i18n::trn("{} file", "{} files", progress_.files));
    if (!data_)
        return {};
    if (data_->remote)
        return std::string{i18n::tr("Network folders are not scanned")};
    if (!data_->complete())
        return i18n::trf("Partial result · Unreadable: {} · Skipped folders: {}{}",
                         data_->unreadable, data_->skipped_folders,
                         data_->cancelled ? i18n::tr(" · stopped")
                         : data_->limited ? i18n::tr(" · scan limit reached")
                                          : std::string_view{});
    return std::string{i18n::tr("Scan complete · Refresh to check for changes")};
}

void InsightsDialog::key(const ui::KeyEvent& event) {
    if (!open_)
        return;
    if (event.keysym == XKB_KEY_Escape) {
        if (view_ == View::matches)
            back();
        else
            close();
        return;
    }
    if (event.keysym == XKB_KEY_BackSpace ||
        (event.modifiers.alt && event.keysym == XKB_KEY_Left)) {
        back();
        return;
    }
    if (event.modifiers.alt && event.keysym == XKB_KEY_Up) {
        up();
        return;
    }
    if (event.keysym == XKB_KEY_Page_Down || event.keysym == XKB_KEY_Page_Up) {
        page(event.keysym == XKB_KEY_Page_Down ? 1 : -1);
        return;
    }
    if (controls_.empty())
        return;
    auto found = std::ranges::find(controls_, focus_, &Control::id);
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab ||
        event.keysym == XKB_KEY_Down || event.keysym == XKB_KEY_Up) {
        const bool back = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ||
                          event.keysym == XKB_KEY_Up;
        auto index =
            found == controls_.end() ? 0 : static_cast<std::size_t>(found - controls_.begin());
        if (found == controls_.end())
            index = back ? controls_.size() - 1 : 0;
        else
            index = back ? (index + controls_.size() - 1) % controls_.size()
                         : (index + 1) % controls_.size();
        focus_ = controls_[index].id;
        redraw_();
    } else if ((event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
                event.keysym == XKB_KEY_space) &&
               found != controls_.end()) {
        auto action = found->action;
        action();
    }
}

void InsightsDialog::pointer(const ui::PointerEvent& event) {
    if (event.kind == ui::PointerEvent::Kind::motion) {
        std::string hovered;
        for (const auto& control : controls_)
            if (control.bounds.contains(event.x, event.y)) {
                hovered = control.id;
                break;
            }
        if (hover_ != hovered) {
            hover_ = std::move(hovered);
            redraw_();
        }
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y)) {
        const float delta = event.wheel_y != 0 ? event.wheel_y : event.scroll_y;
        if (delta != 0)
            page(delta > 0 ? 1 : -1);
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    for (const auto& control : controls_)
        if (control.bounds.contains(event.x, event.y)) {
            focus_ = control.id;
            auto action = control.action;
            action();
            return;
        }
}

ui::CursorShape InsightsDialog::cursor_at(float x, float y) const {
    return std::ranges::any_of(controls_, [&](const Control& c) { return c.bounds.contains(x, y); })
               ? ui::CursorShape::pointer
               : ui::CursorShape::standard;
}

ui::Rect InsightsDialog::control_bounds(std::string_view id) const {
    const auto found = std::ranges::find(controls_, id, &Control::id);
    return found == controls_.end() ? ui::Rect{} : found->bounds;
}

void InsightsDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette, ui::IconSet& icons,
                          const fs::MimeDatabase& mime) {
    if (!open_)
        return;
    controls_.clear();
    const auto frame = DialogFrame::fit(bounds, 1080, 820, 112);
    card_ = frame.card;
    frame.draw(renderer, bounds, palette, i18n::tr("Folder insights"));
    const auto surface = palette.surface_container_high;
    const auto muted = theme::readable_foreground(palette.on_surface_variant, surface);
    const float left = frame.body.x + 16, width = std::max(1.0F, frame.body.width - 32);
    const auto text_width = [&](std::string_view text) {
        return renderer.fonts().layout(text, 13, renderer.scale()).width / renderer.scale();
    };
    const auto control = [&](std::string id, std::string_view label, ui::Rect rect,
                             std::function<void()> action, std::string_view glyph = {},
                             bool selected = false, bool enabled = true) {
        const bool hot = enabled && hover_ == id;
        const auto fill = mix(surface, palette.primary, selected ? 0.17F : hot ? 0.09F : 0.035F);
        auto ink =
            theme::readable_foreground(selected ? palette.primary : palette.on_surface, fill);
        if (!enabled)
            ink = ink.with_alpha(0.35F);
        renderer.draw_box(rect, {.fill = fill,
                                 .radius = ui::Corners::all(8),
                                 .border_width = enabled && focus_ == id ? 2.0F
                                                 : selected              ? 1.0F
                                                                         : 0,
                                 .border = palette.primary.with_alpha(selected ? 0.55F : 1.0F)});
        const float label_x = glyph.empty()
                                  ? rect.x + std::max(8.0F, (rect.width - text_width(label)) / 2)
                                  : rect.x + 34;
        if (!glyph.empty())
            icons.draw(renderer, glyph, rect.x + 10, rect.y + 8, 16, ink);
        renderer.draw_text(label, label_x, rect.y + 8, 13, ink,
                           std::max(0.0F, rect.right() - label_x - 8));
        if (enabled && rect.bottom() <= frame.footer.bottom())
            controls_.push_back({std::move(id), rect, std::move(action)});
    };
    renderer.push_clip(frame.body);
    float x = left, y = frame.body.y + 12;
    const float back_width = text_width(i18n::tr("Back")) + 48;
    const float up_width = text_width(i18n::tr("Up")) + 48;
    control(
        "back", i18n::tr("Back"), {x, y, back_width, 32}, [this] { back(); }, "chevron-left", false,
        can_back());
    x += back_width + 6;
    control(
        "up", i18n::tr("Up"), {x, y, up_width, 32}, [this] { up(); }, "chevron-up", false,
        can_up());
    x += up_width + 14;
    // Breadcrumbs show the nearest ancestors that fit. The ellipsis navigates to
    // the next hidden ancestor, so even very long paths remain usable.
    std::vector<std::filesystem::path> crumbs;
    auto path = scope_path();
    for (auto parent = path; !parent.empty();) {
        crumbs.push_back(parent);
        const auto next = parent.parent_path();
        if (next == parent)
            break;
        parent = next;
    }
    std::ranges::reverse(crumbs);
    const auto crumb_label = [](const std::filesystem::path& value) {
        return value.has_filename() ? value.filename().string() : value.string();
    };
    std::size_t first = crumbs.size();
    float used = 0;
    const float crumb_space = std::max(0.0F, left + width - x);
    while (first > 0) {
        const float w = std::clamp(text_width(crumb_label(crumbs[first - 1])) + 20, 40.0F, 160.0F);
        if (used + w + 18 + (first > 1 ? 36 : 0) > crumb_space && first != crumbs.size())
            break;
        used += w + 18;
        --first;
    }
    if (first > 0 && crumb_space > 80) {
        control("crumb:more", "…", {x, y, 30, 32},
                [this, target = crumbs[first - 1]] { navigate(target); });
        x += 36;
    }
    for (std::size_t i = first; i < crumbs.size() && x < left + width - 12; ++i) {
        const auto label = crumb_label(crumbs[i]);
        const float w =
            std::min(left + width - x, std::clamp(text_width(label) + 20, 40.0F, 160.0F));
        if (i + 1 == crumbs.size())
            renderer.draw_text(label, x + 8, y + 8, 13, palette.on_surface, w - 12,
                               ui::Ellipsis::middle);
        else
            control(std::format("crumb:{}", i), label, {x, y, w, 32},
                    [this, target = crumbs[i]] { navigate(target); });
        x += w;
        if (i + 1 < crumbs.size()) {
            icons.draw(renderer, "chevron-right", x + 1, y + 9, 14, muted);
            x += 18;
        }
    }
    x = left;
    y += 44;
    const auto button = [&](std::string id, std::string_view label, std::function<void()> action,
                            bool selected = false, std::string_view glyph = {}) {
        const float w =
            std::min(width, std::max(72.0F, text_width(label) + (glyph.empty() ? 24 : 44)));
        if (x > left && x + w > left + width) {
            x = left;
            y += 38;
        }
        const ui::Rect rect{x, y, w, 32};
        if (rect.bottom() <= frame.body.bottom())
            control(std::move(id), label, rect, std::move(action), glyph, selected);
        x += w + 6;
    };
    button(
        "types", i18n::tr("File types"), [this] { set_view(View::types); },
        view_ == View::types || view_ == View::matches, "list-details");
    button(
        "space", i18n::tr("Space map"), [this] { set_view(View::space); }, view_ == View::space,
        "layout-grid");
    x += 12;
    button("count", i18n::tr("File count"), [this] { set_size_measure(false); }, !bytes_);
    button("bytes", i18n::tr("File size"), [this] { set_size_measure(true); }, bytes_);
    x = left;
    y += 38;
    button("recursive",
           options_.recursive ? i18n::tr("Include subfolders: on")
                              : i18n::tr("Include subfolders: off"),
           [this] { set_recursive(!options_.recursive); });
    button("hidden", options_.hidden ? i18n::tr("Hidden files: on") : i18n::tr("Hidden files: off"),
           [this] { set_hidden(!options_.hidden); });
    y += 44;
    const auto files = data_ ? data_->nodes[scope_].files : progress_.files;
    const auto bytes = data_ ? data_->nodes[scope_].bytes : progress_.bytes;
    const bool partial = loading_ || (data_ && data_->nodes[scope_].partial);
    std::string summary =
        std::format("{} · {}", i18n::trn("{} file", "{} files", files), format_size(bytes));
    if (partial)
        summary = i18n::trf("At least {}", summary);
    if (view_ == View::matches)
        summary = std::format("{} · {}", extension_label(extension_),
                              i18n::trf("Matches: {}", rows_.size()));
    const ui::Rect heading{left, y, width, 64};
    const auto heading_fill = mix(surface, palette.secondary, 0.07F);
    renderer.draw_box(heading, {.fill = heading_fill, .radius = ui::Corners::all(10)});
    icons.draw(renderer, "folder", left + 14, y + 16, 32,
               theme::readable_foreground(palette.secondary, heading_fill));
    renderer.draw_text(crumb_label(path), left + 60, y + 10, 17, palette.on_surface, width - 76,
                       ui::Ellipsis::middle);
    renderer.draw_text(summary, left + 60, y + 36, 13, muted, width - 76);
    y += 76;
    if (view_ == View::space && partial && !loading_) {
        renderer.draw_text(i18n::tr("Partial map · Only measured files have area"), left, y, 12,
                           muted, width);
        y += 24;
    }
    ui::Rect available{left, y, width, std::max(0.0F, frame.body.bottom() - y - 12)};
    list_ = available;
    const auto row_icon = [&](const Row& row, const ui::Rect& icon, ui::Color color) {
        const auto type =
            row.directory
                ? fs::MimeDatabase::kDirectory
                : mime.by_name(view_ == View::types ? "file" + row.extension : row.label).mime;
        icons.draw_file(renderer, mime.name(type), fs::icon_name(mime, type), icon.x, icon.y,
                        icon.width, color);
    };
    if (view_ == View::space && !rows_.empty() && available.height >= 150) {
        const bool wide = width >= 760;
        const ui::Rect map{available.x, available.y, wide ? width * 0.55F : width,
                           wide ? available.height : available.height * 0.44F};
        renderer.draw_box(map,
                          {.fill = palette.surface_container_low, .radius = ui::Corners::all(12)});
        if (wide) {
            list_.x = map.right() + 16;
            list_.width = available.right() - list_.x;
        } else {
            list_.y = map.bottom() + 12;
            list_.height = available.bottom() - list_.y;
        }
        constexpr std::size_t max_tiles = 48;
        std::vector<double> weights;
        const std::size_t shown = std::min(max_tiles, rows_.size());
        for (std::size_t i = 0; i < shown; ++i)
            weights.push_back(static_cast<double>(bytes_ ? rows_[i].bytes : rows_[i].files));
        if (shown < rows_.size()) {
            double rest = 0;
            for (std::size_t i = shown; i < rows_.size(); ++i)
                rest += static_cast<double>(bytes_ ? rows_[i].bytes : rows_[i].files);
            weights.push_back(rest);
        }
        const auto tiles = insight_treemap(weights, map.inset(4));
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const auto cell = tiles[i];
            if (cell.empty())
                continue;
            const auto rect = cell.inset(std::min(3.0F, std::min(cell.width, cell.height) / 8));
            const auto id = std::format("tile:{}", i);
            const bool folder = i >= shown || rows_[i].directory;
            const auto accent = i < shown ? row_color(rows_[i], palette) : palette.secondary;
            const bool hot = hover_ == id || focus_ == id;
            const auto fill = mix(palette.surface_container_low, accent, hot ? 0.34F : 0.25F);
            const auto ink =
                theme::readable_foreground(accent, folder ? fill : palette.surface_container_low);
            renderer.push_clip(rect);
            if (folder) {
                renderer.draw_box(rect, {.fill = fill,
                                         .radius = ui::Corners::all(8),
                                         .border_width = hot ? 2.0F : 1.0F,
                                         .border = accent.with_alpha(hot ? 0.8F : 0.22F)});
                if (rect.width > 140 && rect.height > 140)
                    icons.draw(renderer, "folder", rect.right() - 108, rect.bottom() - 102, 92,
                               accent.with_alpha(0.1F));
            } else if (hot) {
                renderer.draw_box(rect, {.fill = mix(palette.surface_container_low, accent, 0.08F),
                                         .radius = ui::Corners::all(8),
                                         .border_width = 1,
                                         .border = accent.with_alpha(0.5F)});
            }
            const auto label =
                i < shown ? rows_[i].label : i18n::trf("{} more", rows_.size() - shown);
            auto amount =
                bytes_ ? format_size(static_cast<std::uint64_t>(weights[i]))
                       : i18n::trn("{} file", "{} files", static_cast<std::uint64_t>(weights[i]));
            if (i < shown ? rows_[i].partial : partial)
                amount = i18n::trf("≥ {}", amount);
            if (folder && rect.width >= 80 && rect.height >= 32) {
                icons.draw(renderer, "folder", rect.x + 10, rect.y + 8, 20, ink);
                renderer.draw_text(label, rect.x + 38, rect.y + 10, 13, palette.on_surface,
                                   rect.width - 54);
                if (rect.height >= 60)
                    renderer.draw_text(amount, rect.x + 12, rect.y + 40, 12,
                                       theme::readable_foreground(muted, fill), rect.width - 24);
            } else if (!folder && rect.width >= 22 && rect.height >= 22) {
                const bool labelled = rect.width >= 84 && rect.height >= 96;
                const float size =
                    std::min({96.0F, rect.width - 8, rect.height - (labelled ? 54 : 8)});
                const float icon_y = rect.y + (rect.height - size - (labelled ? 42 : 0)) / 2;
                row_icon(rows_[i], {rect.x + (rect.width - size) / 2, icon_y, size, size}, ink);
                if (labelled) {
                    const float label_y = icon_y + size + 8;
                    const float label_width = std::min(rect.width - 16, text_width(label));
                    const float amount_width =
                        std::min(rect.width - 16,
                                 renderer.fonts().layout(amount, 12, renderer.scale()).width /
                                     renderer.scale());
                    renderer.draw_text(label, rect.x + (rect.width - label_width) / 2, label_y, 13,
                                       palette.on_surface, label_width, ui::Ellipsis::middle);
                    renderer.draw_text(amount, rect.x + (rect.width - amount_width) / 2,
                                       label_y + 20, 12, muted, amount_width);
                }
            }
            renderer.pop_clip();
            controls_.push_back({id, cell, [this, i, shown] {
                                     if (i < shown)
                                         activate_row(i);
                                     else {
                                         offset_ = shown;
                                         controls_.clear();
                                         redraw_();
                                     }
                                 }});
        }
        if (std::ranges::all_of(weights, [](double value) { return value == 0; }))
            renderer.draw_text(partial && files == 0 ? i18n::tr("No files measured yet")
                                                     : i18n::tr("No area for zero values"),
                               map.x + 12, map.y + 12, 13, muted, map.width - 24);
    }
    const bool shares = view_ == View::types && list_.width >= 520;
    const float count_x = shares ? 0.48F : 0.55F, size_x = shares ? 0.64F : 0.76F;
    renderer.draw_text(view_ == View::types ? i18n::tr("Extension") : i18n::tr("Name"),
                       list_.x + 10, list_.y, 12, muted, list_.width * (count_x - 0.03F));
    renderer.draw_text(i18n::tr("Files"), list_.x + list_.width * count_x, list_.y, 12, muted,
                       list_.width * 0.17F);
    renderer.draw_text(i18n::tr("File size"), list_.x + list_.width * size_x, list_.y, 12, muted,
                       list_.width * (shares ? 0.18F : 0.24F));
    if (shares)
        renderer.draw_text(partial ? i18n::tr("Share so far") : i18n::tr("Share"),
                           list_.x + list_.width * 0.84F, list_.y, 12, muted, list_.width * 0.16F);
    page_size_ =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::max(0.0F, list_.height - 28) / 44));
    // A resize must never leave the remembered page beyond the visible result range.
    if (!rows_.empty())
        offset_ = std::min(offset_, rows_.size() - 1);
    for (std::size_t i = offset_; i < rows_.size() && i < offset_ + page_size_; ++i) {
        const auto& row = rows_[i];
        const ui::Rect rect{list_.x, list_.y + 28 + static_cast<float>(i - offset_) * 44,
                            list_.width, 40};
        if (rect.bottom() > frame.body.bottom())
            break;
        const auto id = std::format("row:{}", i);
        const auto accent = row_color(row, palette);
        const bool hot = hover_ == id || focus_ == id;
        renderer.draw_box(rect, {.fill = mix(surface, accent, hot ? 0.15F : 0.045F),
                                 .radius = ui::Corners::all(7),
                                 .border_width = focus_ == id ? 1.0F : 0,
                                 .border = accent});
        const auto label =
            view_ == View::matches && data_ ? data_->relative_path(row.node).string() : row.label;
        row_icon(row, {rect.x + 10, rect.y + 10, 20, 20},
                 theme::readable_foreground(accent, surface));
        renderer.draw_text(label, rect.x + 40, rect.y + 10, 13, palette.on_surface,
                           std::max(0.0F, rect.width * (count_x - 0.03F) - 40),
                           ui::Ellipsis::middle);
        const auto total = bytes_ ? bytes : files;
        const auto value = bytes_ ? row.bytes : row.files;
        const float share =
            total ? static_cast<float>(static_cast<double>(value) / static_cast<double>(total)) : 0;
        if (view_ == View::types || view_ == View::space)
            renderer.draw_box(
                {rect.x + 40, rect.bottom() - 5, (rect.width * (count_x - 0.03F) - 40) * share, 2},
                {.fill = accent, .radius = ui::Corners::all(1)});
        if (shares)
            renderer.draw_text(std::format("{:.1f}%", share * 100), rect.x + rect.width * 0.84F,
                               rect.y + 10, 12, muted, rect.width * 0.16F);
        const bool unscanned = row.directory && !options_.recursive;
        const bool unknown = row.partial && row.files == 0;
        renderer.draw_text(unscanned     ? "—"
                           : unknown     ? std::string{i18n::tr("Unknown")}
                           : row.partial ? i18n::trf("≥ {}", row.files)
                                         : std::to_string(row.files),
                           rect.x + rect.width * count_x, rect.y + 10, 12, muted,
                           rect.width * (size_x - count_x - 0.02F));
        const auto size = unscanned     ? std::string{"—"}
                          : unknown     ? std::string{i18n::tr("Unknown")}
                          : row.partial ? i18n::trf("≥ {}", format_size(row.bytes))
                                        : format_size(row.bytes);
        renderer.draw_text(size, rect.x + rect.width * size_x, rect.y + 10, 12, muted,
                           rect.width * (shares ? 0.18F : 0.24F));
        controls_.push_back({id, rect, [this, i] { activate_row(i); }});
    }
    if (rows_.empty())
        renderer.draw_text(loading_  ? i18n::tr("Counting…")
                           : partial ? i18n::tr("No files measured yet")
                                     : i18n::tr("No files in this scope"),
                           list_.x + 10, list_.y + 36, 13, muted, list_.width - 20);
    renderer.pop_clip();
    renderer.push_clip(frame.footer);
    renderer.draw_text(status(), left, frame.footer.y + 8, 11, muted, width);
    renderer.draw_text(
        i18n::tr("File sizes per name, including hard links · Links are not followed"), left,
        frame.footer.y + 26, 11, muted, width);
    x = left;
    y = frame.footer.y + 48;
    const auto footer_button = [&](std::string id, std::string_view label,
                                   std::function<void()> action, bool enabled = true) {
        const float w = std::min(128.0F, width / 5 - 6);
        control(std::move(id), label, {x, y, w, 32}, std::move(action), {}, false, enabled);
        x += w + 6;
    };
    footer_button("close", i18n::tr("Close"), [this] { close(); });
    footer_button(
        "refresh", loading_ ? i18n::tr("Stop") : i18n::tr("Refresh"),
        [this] {
            if (loading_)
                stop();
            else
                refresh();
        },
        !loading_ || !stop_.cancelled());
    footer_button("reveal", i18n::tr("Show folder"), [this] { reveal(scope_); }, data_ != nullptr);
    footer_button("previous", i18n::tr("Previous"), [this] { page(-1); }, offset_ > 0);
    footer_button(
        "next", i18n::tr("Next"), [this] { page(1); }, offset_ + page_size_ < rows_.size());
    if (!rows_.empty())
        renderer.draw_text(i18n::trf("{}–{} of {} · Select a row to explore", offset_ + 1,
                                     std::min(offset_ + page_size_, rows_.size()), rows_.size()),
                           left, frame.footer.y + 86, 11, muted, width);
    renderer.pop_clip();
}

}  // namespace rummage::views
