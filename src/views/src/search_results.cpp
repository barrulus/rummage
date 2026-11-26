#include "rummage/views/search_results.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {
using Action = SearchResults::Action;
constexpr std::array toolbar{std::pair{Action::copy, i18n::mark("Copy")},
                             std::pair{Action::cut, i18n::mark("Cut")},
                             std::pair{Action::trash, i18n::mark("Move to Trash")},
                             std::pair{Action::rename, i18n::mark("Rename…")},
                             std::pair{Action::preview, i18n::mark("Quick Look")},
                             std::pair{Action::reveal, i18n::mark("Show in Folder")},
                             std::pair{Action::save, i18n::mark("Save Search…")},
                             std::pair{Action::saved, i18n::mark("Saved Searches…")},
                             std::pair{Action::refresh, i18n::mark("Refresh")},
                             std::pair{Action::stop, i18n::mark("Stop")},
                             std::pair{Action::details, i18n::mark("Copy Search Details")},
                             std::pair{Action::close, i18n::mark("Close")}};
}  // namespace

SearchResults::SearchResults(core::WorkerPool& pool, std::function<void()> redraw,
                             std::function<void(Action)> action)
    : pool_{pool}, redraw_{std::move(redraw)}, action_{std::move(action)} {}

void SearchResults::open(const std::filesystem::path& root, std::string query, bool hidden) {
    close();
    open_ = true;
    focus_ = -1;
    first_ = 0;
    query_.set_text(std::move(query));
    finder_ = std::make_unique<FileFinder>(pool_, [this] { updated(); });
    finder_->set_query(query_.text());
    if (root == "rummage:recent")
        finder_->start_recent();
    else
        finder_->start(root, hidden);
    redraw_();
}

SearchResults::Snapshot SearchResults::snapshot() const {
    if (restoring_)
        return *restoring_;
    return {.root = finder_ ? (finder_->recent() ? std::filesystem::path{"rummage:recent"}
                                                 : finder_->root())
                            : std::filesystem::path{},
            .query = query_.text(),
            .hidden = finder_ && finder_->hidden(),
            .selected = selected_,
            .cursor = cursor_,
            .anchor = anchor_,
            .first = first_};
}

void SearchResults::restore(const Snapshot& saved) {
    open(saved.root, saved.query, saved.hidden);
    restoring_ = saved;
    focus_ = -2;
}

void SearchResults::close() {
    restoring_.reset();
    open_ = false;
    finder_.reset();
    selected_.clear();
    cursor_.clear();
    anchor_.clear();
    message_.clear();
    controls_.clear();
    redraw_();
}

void SearchResults::refresh() {
    if (!finder_)
        return;
    selected_.clear();
    cursor_.clear();
    anchor_.clear();
    first_ = 0;
    finder_->refresh();
    redraw_();
}

void SearchResults::set_query(std::string query) {
    query_.set_text(std::move(query));
    selected_.clear();
    cursor_.clear();
    anchor_.clear();
    first_ = 0;
    message_.clear();
    if (finder_)
        finder_->set_query(query_.text());
    redraw_();
}

const std::vector<FoundFile>& SearchResults::rows() const {
    static const std::vector<FoundFile> empty;
    return finder_ ? finder_->results() : empty;
}

void SearchResults::updated() {
    if (restoring_) {
        if (loading()) {
            redraw_();
            return;
        }
        selected_ = std::move(restoring_->selected);
        cursor_ = std::move(restoring_->cursor);
        anchor_ = std::move(restoring_->anchor);
        first_ = restoring_->first;
        restoring_.reset();
    }
    const auto exists = [&](const std::string& path) {
        return std::ranges::find(rows(), path, &FoundFile::path) != rows().end();
    };
    std::erase_if(selected_, [&](const auto& path) { return !exists(path); });
    if (!exists(cursor_)) {
        const bool initial = cursor_.empty();
        cursor_ = rows().empty() ? "" : rows().front().path;
        if (initial && selected_.empty() && !cursor_.empty())
            selected_.insert(cursor_);
    }
    first_ = std::min(first_, rows().size() > visible_ ? rows().size() - visible_ : 0);
    redraw_();
}

std::size_t SearchResults::cursor_index() const {
    const auto found = std::ranges::find(rows(), cursor_, &FoundFile::path);
    return found == rows().end() ? 0 : static_cast<std::size_t>(found - rows().begin());
}

void SearchResults::reveal_cursor() {
    const auto row = cursor_index();
    if (row < first_)
        first_ = row;
    else if (row >= first_ + visible_)
        first_ = row - visible_ + 1;
}

void SearchResults::select(std::size_t row, ui::Modifiers modifiers) {
    if (row >= rows().size())
        return;
    focus_ = -2;
    cursor_ = rows()[row].path;
    if (modifiers.shift) {
        const auto anchor = std::ranges::find(rows(), anchor_, &FoundFile::path);
        const auto start =
            anchor == rows().end() ? row : static_cast<std::size_t>(anchor - rows().begin());
        if (!modifiers.ctrl)
            selected_.clear();
        for (auto i = std::min(start, row); i <= std::max(start, row); ++i)
            selected_.insert(rows()[i].path);
    } else if (modifiers.ctrl) {
        if (!selected_.erase(cursor_))
            selected_.insert(cursor_);
        anchor_ = cursor_;
    } else {
        selected_ = {cursor_};
        anchor_ = cursor_;
    }
    reveal_cursor();
    redraw_();
}

void SearchResults::select_all() {
    for (const auto& row : rows())
        selected_.insert(row.path);
    focus_ = -2;
    redraw_();
}

std::vector<std::filesystem::path> SearchResults::chosen() const {
    std::vector<std::filesystem::path> result;
    if (!finder_)
        return result;
    for (const auto& row : rows()) {
        if (!selected_.contains(row.path))
            continue;
        // If a directory and one of its descendants are selected, act on the directory once.
        const bool covered = std::ranges::any_of(rows(), [&](const auto& parent) {
            return parent.directory && selected_.contains(parent.path) &&
                   row.path.starts_with(parent.path + "/");
        });
        if (!covered)
            result.push_back(finder_->root() / row.path);
    }
    return result;
}

bool SearchResults::enabled(Action action) const {
    if (finder_ && finder_->recent() &&
        (action == Action::save || action == Action::saved || action == Action::details))
        return false;
    if (action == Action::stop)
        return loading();
    if (action == Action::copy || action == Action::cut || action == Action::trash)
        return !chosen().empty();
    if (action == Action::rename || action == Action::preview || action == Action::reveal ||
        action == Action::open)
        return chosen().size() == 1;
    return true;
}

void SearchResults::run(Action action) {
    if (!enabled(action))
        return;
    if (action == Action::refresh) {
        refresh();
        return;
    }
    if (action == Action::stop) {
        if (finder_)
            finder_->stop();
        return;
    }
    action_(action);
}

void SearchResults::move(int delta, ui::Modifiers modifiers) {
    if (rows().empty())
        return;
    const auto row = static_cast<std::size_t>(
        std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(cursor_index()) + delta, 0,
                                   static_cast<std::ptrdiff_t>(rows().size() - 1)));
    if (modifiers.ctrl && !modifiers.shift) {
        cursor_ = rows()[row].path;
        focus_ = -2;
        reveal_cursor();
        redraw_();
    } else
        select(row, modifiers);
}

void SearchResults::key(const ui::KeyEvent& event) {
    if (event.keysym == XKB_KEY_Escape) {
        run(Action::close);
        return;
    }
    if (event.keysym == XKB_KEY_F5) {
        run(Action::refresh);
        return;
    }
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_s || event.keysym == XKB_KEY_S)) {
        run(event.modifiers.shift ? Action::saved : Action::save);
        return;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        const int count = static_cast<int>(toolbar.size());
        int slot = focus_ == -1 ? 0 : focus_ == -2 ? count + 1 : focus_ + 1;
        const int direction =
            event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ? -1 : 1;
        do {
            slot = (slot + direction + count + 2) % (count + 2);
        } while (slot > 0 && slot <= count &&
                 !enabled(toolbar[static_cast<std::size_t>(slot - 1)].first));
        focus_ = slot == 0 ? -1 : slot == count + 1 ? -2 : slot - 1;
        redraw_();
        return;
    }
    if (focus_ == -1) {
        if (event.keysym == XKB_KEY_Down || event.keysym == XKB_KEY_Return) {
            select(cursor_index());
            return;
        }
        const auto old = query_.text();
        query_.key(event);
        if (old != query_.text()) {
            selected_.clear();
            cursor_.clear();
            anchor_.clear();
            first_ = 0;
            message_.clear();
            if (finder_)
                finder_->set_query(query_.text());
        }
        redraw_();
        return;
    }
    if (focus_ >= 0) {
        if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_space)
            run(toolbar[static_cast<std::size_t>(focus_)].first);
        return;
    }
    if (event.modifiers.ctrl) {
        switch (event.keysym) {
            case XKB_KEY_a:
            case XKB_KEY_A:
                select_all();
                return;
            case XKB_KEY_c:
            case XKB_KEY_C:
                run(Action::copy);
                return;
            case XKB_KEY_x:
            case XKB_KEY_X:
                run(Action::cut);
                return;
            case XKB_KEY_z:
            case XKB_KEY_Z:
                run(event.modifiers.shift ? Action::redo : Action::undo);
                return;
            default:
                break;
        }
    }
    switch (event.keysym) {
        case XKB_KEY_Up:
            move(-1, event.modifiers);
            break;
        case XKB_KEY_Down:
            move(1, event.modifiers);
            break;
        case XKB_KEY_Page_Up:
            move(-static_cast<int>(visible_), event.modifiers);
            break;
        case XKB_KEY_Page_Down:
            move(static_cast<int>(visible_), event.modifiers);
            break;
        case XKB_KEY_Home:
            select(0, event.modifiers);
            break;
        case XKB_KEY_End:
            if (!rows().empty())
                select(rows().size() - 1, event.modifiers);
            break;
        case XKB_KEY_Return:
            run(event.modifiers.shift ? Action::reveal : Action::open);
            break;
        case XKB_KEY_space:
            run(Action::preview);
            break;
        case XKB_KEY_Delete:
            if (!event.modifiers.shift)
                run(Action::trash);
            break;
        case XKB_KEY_F2:
            run(Action::rename);
            break;
        default:
            break;
    }
}

void SearchResults::paste(std::string_view text) {
    if (!editing())
        return;
    query_.insert(text);
    selected_.clear();
    cursor_.clear();
    anchor_.clear();
    first_ = 0;
    if (finder_)
        finder_->set_query(query_.text());
    redraw_();
}

std::string SearchResults::copy_query(bool cut) {
    if (!editing())
        return {};
    const auto text = cut ? query_.cut() : query_.selected_text();
    if (cut && finder_) {
        selected_.clear();
        finder_->set_query(query_.text());
    }
    redraw_();
    return text;
}

void SearchResults::pointer(const ui::PointerEvent& event) {
    if (editing() && event.kind != ui::PointerEvent::Kind::press && query_.pointer(event)) {
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y)) {
        wheel_rest_ += event.wheel_y != 0 ? event.wheel_y * 3 : event.scroll_y / row_height_;
        const auto delta = static_cast<int>(wheel_rest_);
        wheel_rest_ -= static_cast<float>(delta);
        first_ = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(first_) + delta, 0,
            static_cast<std::ptrdiff_t>(rows().size() > visible_ ? rows().size() - visible_ : 0)));
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    if (query_.bounds().contains(event.x, event.y)) {
        focus_ = -1;
        query_.pointer(event);
        redraw_();
        return;
    }
    for (std::size_t i = 0; i < controls_.size(); ++i)
        if (controls_[i].bounds.contains(event.x, event.y)) {
            const auto control = controls_[i];
            if (control.enabled) {
                focus_ = static_cast<int>(i);
                run(control.action);
            }
            return;
        }
    if (list_.contains(event.x, event.y)) {
        const auto row = first_ + static_cast<std::size_t>((event.y - list_.y) / row_height_);
        select(row, event.modifiers);
        if (event.click_count == 2 && row < rows().size())
            run(Action::open);
    }
}

ui::Rect SearchResults::control_bounds(Action action) const {
    for (const auto& control : controls_)
        if (control.action == action)
            return control.bounds;
    return {};
}

ui::CursorShape SearchResults::cursor_at(float x, float y) const {
    if (query_.bounds().contains(x, y))
        return ui::CursorShape::text;
    for (const auto& control : controls_)
        if (control.enabled && control.bounds.contains(x, y))
            return ui::CursorShape::pointer;
    return ui::CursorShape::standard;
}

void SearchResults::draw(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette) {
    if (!open_ || !finder_)
        return;
    renderer.fill_rect(bounds, palette.surface);
    renderer.push_clip(bounds);
    const float left = bounds.x + 16, width = std::max(1.0F, bounds.width - 32);
    renderer.draw_text(i18n::tr(finder_->recent() ? "Recent Files" : "Search Results"), left,
                       bounds.y + 12, 18, palette.on_surface, width);
    renderer.draw_text(finder_->recent()
                           ? std::string{i18n::tr("Files opened in Rummage · newest first")}
                           : finder_->root().string(),
                       left, bounds.y + 38, 12, palette.on_surface_variant, width);
    query_.draw(renderer, {left, bounds.y + 60, width, 34}, 14,
                {.background = palette.surface_container,
                 .border = editing() ? palette.primary : palette.outline_variant,
                 .text = palette.on_surface,
                 .selection = palette.primary_container,
                 .caret = editing() ? palette.primary : palette.primary.with_alpha(0)});
    controls_.clear();
    float x = left, y = bounds.y + 102;
    for (std::size_t i = 0; i < toolbar.size(); ++i) {
        const auto [action, label] = toolbar[i];
        if (finder_->recent() &&
            (action == Action::save || action == Action::saved || action == Action::details)) {
            controls_.push_back({action, {}, false});
            continue;
        }
        const auto translated = i18n::tr(label);
        const float w =
            std::min(width, renderer.fonts().layout(translated, 12, renderer.scale()).width /
                                    renderer.scale() +
                                20);
        if (x + w > left + width && x > left) {
            x = left;
            y += 32;
        }
        const ui::Rect rect{x, y, w, 28};
        const bool available = enabled(action);
        renderer.draw_box(rect, {.fill = palette.surface_container_highest,
                                 .radius = {},
                                 .border_width = focus_ == static_cast<int>(i) ? 2.0F : 0.0F,
                                 .border = palette.primary});
        renderer.draw_text(
            translated, x + 10, y + 6, 12,
            available ? palette.on_surface : palette.on_surface_variant.with_alpha(0.45F), w - 20);
        controls_.push_back({action, rect, available});
        x += w + 6;
    }
    y += 38;
    list_ = {left, y, width, std::max(0.0F, bounds.bottom() - y - 76)};
    row_height_ = finder_->specification().content ? 72 : 52;
    visible_ = std::max<std::size_t>(1, static_cast<std::size_t>(list_.height / row_height_));
    first_ = std::min(first_, rows().size() > visible_ ? rows().size() - visible_ : 0);
    renderer.push_clip(list_);
    for (std::size_t i = first_; i < rows().size() && i < first_ + visible_; ++i) {
        const auto& row = rows()[i];
        const bool chosen = selected_.contains(row.path);
        const ui::Rect rect{left, y + static_cast<float>(i - first_) * row_height_, width,
                            row_height_ - 2};
        if (chosen)
            renderer.fill_rect(rect, palette.primary_container);
        else if (i % 2 == 0)
            renderer.fill_rect(rect, palette.surface_container);
        if (row.path == cursor_ && focus_ == -2)
            renderer.draw_box(rect, {.fill = palette.surface.with_alpha(0),
                                     .radius = {},
                                     .border_width = 1,
                                     .border = palette.primary});
        const auto ink = chosen ? palette.on_primary_container : palette.on_surface;
        const std::filesystem::path relative{row.path};
        renderer.draw_text(relative.filename().string(), left + 10, rect.y + 5, 14, ink,
                           width - 20);
        const auto parent = finder_->root() / relative.parent_path();
        renderer.draw_text(parent.string() + (finder_->recent() ? " · " + row.context : ""),
                           left + 10, rect.y + 26, 11, ink.with_alpha(0.75F), width - 20);
        if (row.line)
            renderer.draw_text(std::format("{}: {}", row.line, row.context), left + 10, rect.y + 46,
                               11, ink.with_alpha(0.8F), width - 20);
    }
    renderer.pop_clip();
    const float footer = bounds.bottom() - 66;
    renderer.draw_text(i18n::trf("Selected: {} · Results: {}", chosen().size(), rows().size()),
                       left, footer, 12, palette.on_surface, width);
    renderer.draw_text(finder_->status(), left, footer + 20, 12, palette.on_surface_variant, width);
    renderer.draw_text(
        message_.empty()
            ? i18n::tr("Ctrl/Shift-click selects · Space previews · F2 renames one item")
            : message_,
        left, footer + 40, 12, palette.on_surface_variant, width);
    renderer.pop_clip();
}
}  // namespace rummage::views
