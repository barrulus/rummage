#include "rummage/views/breadcrumb.hpp"

#include <algorithm>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/fs/path_completion.hpp"
#include "rummage/fs/trash.hpp"

namespace rummage::views {
namespace {

constexpr float kPadding = 16.0F;  // either end of the bar
constexpr float kInset = 6.0F;     // either side of a step, inside its hover box
constexpr float kIconSize = 16.0F;
constexpr float kChevronSize = 12.0F;
constexpr float kChevronGap = 2.0F;  // either side of a chevron
constexpr float kBoxHeight = 26.0F;
constexpr float kTextSize = 14.0F;
constexpr std::string_view kEllipsis = "…";

}  // namespace

std::vector<Crumb> crumbs_of(const std::filesystem::path& folder,
                             const std::filesystem::path& home) {
    if (fs::is_trash(folder)) {
        return {{.path = folder, .label = {}, .icon = "trash"},
                {.path = folder, .label = std::string{i18n::tr("Trash")}, .icon = {}}};
    }
    const std::filesystem::path normal = folder.lexically_normal();
    std::filesystem::path base{"/"};
    std::string_view icon = "server";
    if (!home.empty() && home != "/") {
        const std::filesystem::path relative = normal.lexically_relative(home.lexically_normal());
        if (!relative.empty() && *relative.begin() != "..") {
            base = home.lexically_normal();
            icon = "home";
        }
    }
    std::vector<Crumb> crumbs{{.path = base, .label = {}, .icon = icon}};
    std::filesystem::path path = base;
    for (const std::filesystem::path& part : normal.lexically_relative(base)) {
        if (part.empty() || part == ".") {
            continue;
        }
        path /= part;
        crumbs.push_back({.path = path, .label = part.string(), .icon = {}});
    }
    return crumbs;
}

Breadcrumb::Breadcrumb(std::function<void()> redraw, Go go, std::filesystem::path home,
                       core::WorkerPool* pool)
    : pool_{pool}, redraw_{std::move(redraw)}, go_{std::move(go)}, home_{std::move(home)} {}

void Breadcrumb::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                      ui::IconSet& icons, const Folder& folder) {
    bounds_ = bounds;
    if (path_ != folder.path()) {
        path_ = folder.path();
        cancel_edit();
        error_.clear();
    }
    if (editing_) {
        field_.draw(
            renderer,
            {bounds.x + 8, bounds.y + 5, std::max(1.0F, bounds.width - 16), bounds.height - 10},
            kTextSize,
            {palette.surface, error_.empty() ? palette.primary : palette.error, palette.on_surface,
             palette.primary_container, palette.primary});
        return;
    }
    std::vector<Crumb> crumbs;
    if (folder.is_collection())
        crumbs.push_back({.path = {}, .label = folder.collection_label(), .icon = {}});
    else
        crumbs = crumbs_of(folder.path(), home_);
    if (crumbs != crumbs_) {
        crumbs_ = std::move(crumbs);
        hovered_.reset();
    }

    const float top = bounds.y + ((kTitleHeight - kBoxHeight) / 2.0F);

    // Every step's width, then which fit: the first, "…", and as many of the last as there is
    // room for (the last one always, cut in the middle when even it is too long).
    const auto width_of = [&](const Crumb& crumb) {
        return (crumb.label.empty() ? kIconSize : text_width(renderer, crumb.label, kTextSize)) +
               (2.0F * kInset);
    };
    const float separator = kChevronSize + (2.0F * kChevronGap);
    const float room = std::max(0.0F, bounds.width - (2.0F * kPadding));
    std::vector<float> widths;
    widths.reserve(crumbs_.size());
    float total = 0.0F;
    for (const Crumb& crumb : crumbs_) {
        widths.push_back(width_of(crumb));
        total += widths.back() + (widths.size() > 1 ? separator : 0.0F);
    }
    std::size_t first_tail = 1;  // crumbs [1, first_tail) are left out
    if (total > room && crumbs_.size() > 2) {
        const float ellipsis = text_width(renderer, kEllipsis, kTextSize) + (2.0F * kInset);
        float used = widths.front() + separator + ellipsis;
        first_tail = crumbs_.size() - 1;
        used += separator + widths.back();
        while (first_tail > 1 && used + separator + widths[first_tail - 1] <= room) {
            --first_tail;
            used += separator + widths[first_tail];
        }
    }

    shown_.clear();
    float x = bounds.x + kPadding - kInset;
    const auto chevron = [&] {
        icons.draw(renderer, "chevron-right", x + kChevronGap,
                   top + ((kBoxHeight - kChevronSize) / 2.0F), kChevronSize,
                   palette.on_surface_variant);
        x += separator;
    };
    for (std::size_t i = 0; i < crumbs_.size(); ++i) {
        if (i == 1 && first_tail > 1) {
            const float width = text_width(renderer, kEllipsis, kTextSize) + (2.0F * kInset);
            chevron();
            renderer.draw_text(kEllipsis, x + kInset, bounds.y + 12.0F, kTextSize,
                               palette.on_surface_variant);
            shown_.push_back({.crumb = std::nullopt, .rect = {x, top, width, kBoxHeight}});
            x += width;
            i = first_tail - 1;
            continue;
        }
        if (i > 0) {
            chevron();
        }
        const Crumb& crumb = crumbs_[i];
        const bool last = i + 1 == crumbs_.size();
        const float width =
            std::min(widths[i], std::max((2.0F * kInset) + 1.0F, bounds.x + kPadding + room - x));
        const ui::Rect box{x, top, width, kBoxHeight};
        const bool drop = drop_target_ == crumb.path;
        if (drop) {
            renderer.draw_box(box, {.fill = palette.primary_container,
                                    .radius = {},
                                    .border_width = 1.5F,
                                    .border = palette.primary});
        } else if (hovered_ == i && !last) {
            renderer.draw_box(box, {.fill = palette.surface_container_high, .radius = {}});
        }
        ui::Color color = last || hovered_ == i ? palette.on_surface : palette.on_surface_variant;
        if (drop) {
            color = palette.on_primary_container;
        }
        if (crumb.label.empty()) {
            icons.draw(renderer, crumb.icon, x + kInset, top + ((kBoxHeight - kIconSize) / 2.0F),
                       kIconSize,
                       last && !drop
                           ? theme::readable_foreground(palette.primary, palette.surface_container)
                           : color);
        } else {
            renderer.draw_text(crumb.label, x + kInset, bounds.y + 12.0F, kTextSize, color,
                               width - (2.0F * kInset), ui::Ellipsis::middle);
        }
        shown_.push_back({.crumb = i, .rect = box});
        x += width;
    }

    renderer.fill_rect(
        {bounds.x, bounds.y + kTitleHeight - 1.0F, bounds.width, 1.0F / renderer.scale()},
        palette.outline_variant);
}

std::optional<std::filesystem::path> Breadcrumb::path_at(float x, float y) const {
    if (editing_)
        return std::nullopt;
    const std::optional<std::size_t> crumb = crumb_at(x, y);
    return crumb ? std::optional{crumbs_[*crumb].path} : std::nullopt;
}

void Breadcrumb::set_drop_target(std::optional<std::filesystem::path> path) {
    if (path != drop_target_) {
        drop_target_ = std::move(path);
        redraw_();
    }
}

std::optional<std::size_t> Breadcrumb::crumb_at(float x, float y) const {
    for (const Shown& shown : shown_) {
        if (shown.crumb && x >= shown.rect.x && x < shown.rect.right() && y >= shown.rect.y &&
            y < shown.rect.bottom()) {
            return shown.crumb;
        }
    }
    return std::nullopt;
}

bool Breadcrumb::clickable_at(float x, float y) const {
    const std::optional<std::size_t> crumb = crumb_at(x, y);
    return claims(x, y) && (submit_ || (crumb && *crumb + 1 < crumbs_.size()));
}

bool Breadcrumb::claims(float x, float y) const {
    return !bounds_.empty() && x >= bounds_.x && x < bounds_.right() && y >= bounds_.y &&
           y < bounds_.bottom();
}

void Breadcrumb::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    if (editing_) {
        if (field_.pointer(event)) {
            invalidate_completion();
            redraw_();
        }
        return;
    }
    if (submit_ && event.kind == Kind::press && event.button == ui::PointerButton::left &&
        claims(event.x, event.y)) {
        const auto crumb = crumb_at(event.x, event.y);
        if (!crumb || *crumb + 1 == crumbs_.size()) {
            edit(path_);
            return;
        }
    }
    std::optional<std::size_t> over;
    if (event.kind != Kind::leave) {
        over = crumb_at(event.x, event.y);
    }
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left || !over ||
        *over + 1 >= crumbs_.size()) {
        return;  // the last step is the folder shown
    }
    const std::filesystem::path path = crumbs_[*over].path;
    const std::string child = crumbs_[*over + 1].path.filename().string();
    if (go_) {
        go_(path, child);
    }
}

void Breadcrumb::edit(const std::filesystem::path& path) {
    if (!submit_)
        return;
    invalidate_completion();
    path_ = path;
    field_.set_text(path.string());
    field_.select_all();
    error_.clear();
    editing_ = true;
    redraw_();
}

bool Breadcrumb::key(const ui::KeyEvent& event) {
    if (!editing_)
        return false;
    if (event.keysym == XKB_KEY_Escape) {
        cancel_edit();
    } else if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter) {
        invalidate_completion();
        error_ = submit_ ? submit_(field_.text()) : std::string{};
        if (error_.empty() && !submitting_)
            editing_ = false;
        redraw_();
    } else if ((event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) &&
               !event.modifiers.ctrl && !event.modifiers.alt) {
        complete(event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab);
    } else if (event.modifiers.ctrl && (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C ||
                                        event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X)) {
        invalidate_completion();
        const auto [first, last] = field_.selection();
        if (copy_ && first != last)
            copy_(field_.text().substr(first, last - first));
        if (event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X)
            field_.insert({});
        redraw_();
    } else if (field_.key(event)) {
        invalidate_completion();
        error_.clear();
        redraw_();
    }
    return true;
}

std::string Breadcrumb::copy_selection(bool cut) {
    if (!editing_)
        return {};
    auto& field = field_;
    auto text = cut ? field.cut() : field.selected_text();
    if (cut && !text.empty()) {
        invalidate_completion();
        error_.clear();
        redraw_();
    }
    return text;
}

void Breadcrumb::paste(std::string_view text) {
    if (!editing_)
        return;
    invalidate_completion();
    field_.insert(text.substr(0, text.find_first_of("\r\n")));
    error_.clear();
    redraw_();
}

void Breadcrumb::invalidate_completion() {
    ++edit_generation_;
    submitting_ = false;
    completion_job_.cancel();
    completing_ = false;
    completions_.clear();
    completed_text_.clear();
}

void Breadcrumb::choose_completion() {
    if (completions_.empty())
        return;
    const auto count = static_cast<std::ptrdiff_t>(completions_.size());
    completion_index_ = (completion_index_ % count + count) % count;
    completed_text_ = completions_[static_cast<std::size_t>(completion_index_)];
    field_.select_all();
    field_.insert(completed_text_);
    error_.clear();
    if (status_)
        status_(i18n::trf("{} matching paths · Tab / Shift+Tab to cycle", completions_.size()),
                false);
    redraw_();
}

void Breadcrumb::complete(bool reverse) {
    if (!pool_)
        return;
    if (completing_) {
        completion_index_ += reverse ? -1 : 1;
        return;
    }
    if (completions_.size() > 1 && completed_text_ == field_.text()) {
        completion_index_ += reverse ? -1 : 1;
        choose_completion();
        return;
    }
    invalidate_completion();
    completing_ = true;
    completion_index_ = reverse ? -1 : 0;
    const auto text = field_.text();
    const auto generation = edit_generation_;
    completion_job_ = pool_->submit(
        core::Priority::visible,
        [text, path = path_, home = home_](const core::CancelToken& token) {
            return fs::complete_path(text, path, home, token);
        },
        [this, text, generation](fs::PathMatches result) {
            completing_ = false;
            if (!editing_ || generation != edit_generation_ || text != field_.text())
                return;
            completions_ = std::move(result.paths);
            if (completions_.empty()) {
                error_ = result.error.empty() ? std::string{i18n::tr("No matching paths")}
                                              : result.error;
                if (status_)
                    status_(error_, true);
                redraw_();
            } else
                choose_completion();
        });
}

std::vector<std::string> Breadcrumb::shown() const {
    std::vector<std::string> out;
    out.reserve(shown_.size());
    for (const Shown& shown : shown_) {
        if (!shown.crumb) {
            out.emplace_back(kEllipsis);
        } else if (crumbs_[*shown.crumb].label.empty()) {
            out.emplace_back(crumbs_[*shown.crumb].icon);
        } else {
            out.push_back(crumbs_[*shown.crumb].label);
        }
    }
    return out;
}

}  // namespace rummage::views
