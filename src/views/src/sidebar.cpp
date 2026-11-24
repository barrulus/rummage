#include "rummage/views/sidebar.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr float kTop = 8.0F;  // above the first section
constexpr float kSectionHeight = 32.0F;
constexpr float kSectionGap = 8.0F;  // before every section but the first
constexpr float kInset = 8.0F;       // the highlight box inside the sidebar
constexpr float kIconSize = 18.0F;
constexpr float kIconLeft = 18.0F;
constexpr float kTextLeft = 46.0F;
constexpr float kTextSize = 13.0F;
constexpr float kSectionSize = 11.0F;
constexpr float kEjectSize = 16.0F;

std::string_view title_of(fs::Place::Section section) {
    switch (section) {
        case fs::Place::Section::places:
            return i18n::tr("PLACES");
        case fs::Place::Section::bookmarks:
            return i18n::tr("BOOKMARKS");
        case fs::Place::Section::devices:
            return i18n::tr("DEVICES");
    }
    return {};
}

// path is inside (or is) folder, comparing whole components.
bool inside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    auto p = path.begin();
    for (auto f = folder.begin(); f != folder.end(); ++f, ++p) {
        if (f->empty()) {
            continue;  // a trailing slash
        }
        if (p == path.end() || *p != *f) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<std::size_t> place_holding(std::span<const fs::Place> places,
                                         const std::filesystem::path& path) {
    const std::filesystem::path normal = path.lexically_normal();
    std::optional<std::size_t> best;
    std::size_t best_depth = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const std::filesystem::path folder = places[i].path.lexically_normal();
        if (folder.empty() || !inside(normal, folder)) {  // empty: a disk not mounted
            continue;
        }
        const auto depth = static_cast<std::size_t>(std::distance(folder.begin(), folder.end()));
        if (!best || depth > best_depth) {
            best = i;
            best_depth = depth;
        }
    }
    return best;
}

Sidebar::Sidebar(std::function<void()> redraw, std::function<void(ui::CursorShape)> set_cursor,
                 std::function<void(const fs::Place&)> open)
    : redraw_{std::move(redraw)}, set_cursor_{std::move(set_cursor)}, open_{std::move(open)} {}

std::string Sidebar::place_key(const fs::Place& place) {
    if (separator(place))
        return place.uri;
    if (place.section == fs::Place::Section::devices && !place.disk.identity.empty())
        return "uuid:" + place.disk.identity;
    if (place.section == fs::Place::Section::devices)
        return place.disk.volume.empty() ? "mount:" + place.path.string()
                                         : "device:" + place.disk.volume;
    if (place.section == fs::Place::Section::places && !place.icon_key.empty())
        return "place:" + std::string{place.icon_key};
    return place.uri.empty() ? "path:" + place.path.string() : "uri:" + place.uri;
}

void Sidebar::set_organization(std::vector<std::string> order, std::vector<std::string> hidden,
                               std::vector<std::string> collapsed) {
    order_ = std::move(order);
    hidden_ = std::move(hidden);
    collapsed_ = std::move(collapsed);
    rebuild();
}

void Sidebar::hide_place(const fs::Place& place) {
    const auto key = place_key(place);
    if (std::ranges::find(hidden_, key) == hidden_.end() && hidden_.size() < 512)
        hidden_.push_back(key);
    rebuild();
}

void Sidebar::restore_places() {
    hidden_.clear();
    collapsed_.clear();
    rebuild();
}

void Sidebar::move_place(const fs::Place& place, int direction) {
    if (tree_ || (direction != -1 && direction != 1))
        return;
    // Seed unspecified keys without discarding unavailable devices or hidden rows.
    for (const auto& p : sources_) {
        auto key = place_key(p);
        if (std::ranges::find(order_, key) == order_.end() && order_.size() < 512)
            order_.push_back(key);
    }
    std::vector<std::string> visible;
    for (const auto& p : places_)
        if (p.section == place.section)
            visible.push_back(place_key(p));
    const auto at = std::ranges::find(visible, place_key(place));
    if (at == visible.end() || (direction < 0 && at == visible.begin()) ||
        (direction > 0 && std::next(at) == visible.end()))
        return;
    auto a = std::ranges::find(order_, *at), b = std::ranges::find(order_, *(at + direction));
    if (a != order_.end() && b != order_.end())
        std::iter_swap(a, b);
    rebuild();
}

void Sidebar::add_separator(const fs::Place& before) {
    if (tree_ || order_.size() >= 511)
        return;
    for (const auto& p : sources_) {
        auto key = place_key(p);
        if (std::ranges::find(order_, key) == order_.end() && order_.size() < 511)
            order_.push_back(key);
    }
    const auto prefix = "separator:" + std::to_string(static_cast<int>(before.section)) + ":";
    std::size_t id = 0;
    while (std::ranges::find(order_, prefix + std::to_string(id)) != order_.end())
        ++id;
    order_.insert(std::ranges::find(order_, place_key(before)), prefix + std::to_string(id));
    rebuild();
}

void Sidebar::remove_separator(const fs::Place& place) {
    std::erase(order_, place_key(place));
    rebuild();
}

void Sidebar::toggle_section(fs::Place::Section section) {
    const auto key = std::to_string(static_cast<int>(section));
    if (std::erase(collapsed_, key) == 0)
        collapsed_.push_back(key);
    rebuild();
}

void Sidebar::enable_tree(core::EventLoop& loop, core::WorkerPool& pool,
                          fs::DirectoryWatcher& watcher,
                          std::shared_ptr<const fs::MimeDatabase> mime) {
    make_model_ = [&loop, &pool, &watcher, mime] {
        return std::make_unique<fs::DirectoryModel>(loop, pool, watcher, mime);
    };
}

void Sidebar::set_tree(bool enabled) {
    if (tree_ == enabled)
        return;
    confirm_rename();
    tree_ = enabled;
    reveal_pending_ = enabled;
    models_.clear();
    cursor_.reset();
    scroller_.jump_to(0);
    if (tree_)
        expand_folder("/");
    rebuild();
    follow_tree();
}

bool Sidebar::loading() const {
    return std::ranges::any_of(models_, [](const auto& entry) { return entry.second->loading(); });
}

void Sidebar::expand_folder(const std::filesystem::path& path) {
    if (!make_model_ || models_.contains(path))
        return;
    auto model = make_model_();
    model->set_filter(tree_hidden_, {});
    model->set_predicate([](const fs::Entry& entry) { return entry.is_directory(); });
    auto* ptr = model.get();
    models_.emplace(path, std::move(model));
    ptr->set_change_handler([this, path](const fs::ModelChange& change) {
        const auto parent = models_.find(path);
        if (parent != models_.end() && !parent->second->loading() && !change.reset) {
            const auto& entries = parent->second->listing().entries();
            // Do not destroy the model notifying us. Detached descendants release their watches.
            std::erase_if(models_, [&](const auto& item) {
                if (item.first == path || !inside(item.first, path))
                    return false;
                const auto relative = item.first.lexically_relative(path);
                const auto name = relative.begin()->string();
                const auto child = std::ranges::find(entries, name, &fs::Entry::name);
                return child == entries.end() || child->is_link || !child->is_directory();
            });
        }
        follow_tree();
        rebuild();
    });
    ptr->open(path);
}

void Sidebar::toggle_folder(const std::filesystem::path& path) {
    if (!tree_)
        return;
    reveal_pending_ = false;
    const auto at = std::ranges::find(places_, path, &fs::Place::path);
    if (at == places_.end() || links_[static_cast<std::size_t>(at - places_.begin())])
        return;
    if (models_.contains(path)) {
        std::erase_if(models_, [&](const auto& item) { return inside(item.first, path); });
    } else
        expand_folder(path);
    rebuild();
}

void Sidebar::sync_tree(const std::filesystem::path& path, bool hidden) {
    const bool changed = tree_current_ != path;
    tree_current_ = path;
    if (tree_hidden_ != hidden) {
        tree_hidden_ = hidden;
        // set_filter notifies synchronously; do not grow models while iterating.
        const bool previous = std::exchange(following_, true);
        for (auto& [key, model] : models_)
            model->set_filter(hidden, {});
        following_ = previous;
        rebuild();
    }
    if (tree_ && changed) {
        reveal_pending_ = true;
        follow_tree();
        rebuild();
    }
}

void Sidebar::follow_tree() {
    if (!tree_ || following_ || !reveal_pending_ || !tree_current_.is_absolute())
        return;
    following_ = true;
    std::filesystem::path path = "/";
    expand_folder(path);
    std::size_t depth = 0;
    for (const auto& part : tree_current_.relative_path()) {
        if (++depth > 64 || part == "..")
            break;
        const auto parent = models_.find(path);
        if (parent == models_.end() || parent->second->loading())
            break;
        const auto& entries = parent->second->listing().entries();
        auto child = std::ranges::find(entries, part.string(), &fs::Entry::name);
        if (child == entries.end() || child->is_link || !child->is_directory())
            break;
        path /= part;
        if (path != tree_current_)
            expand_folder(path);
    }
    if (path == tree_current_) {
        reveal_pending_ = false;
        reveal_selection_ = true;
    }
    following_ = false;
}

void Sidebar::append_tree(const std::filesystem::path& path, std::size_t depth, bool link) {
    if (depth > 64)
        return;
    places_.push_back(
        {.section = fs::Place::Section::places,
         .label = path == "/" ? std::string{i18n::tr("File System")} : path.filename().string(),
         .path = path,
         .icon = link ? "folder-symlink" : "folder"});
    depths_.push_back(depth);
    links_.push_back(link);
    const auto found = models_.find(path);
    if (found == models_.end() || link)
        return;
    for (const auto& child : found->second->listing().entries()) {
        if (child.is_directory())
            append_tree(path / child.name, depth + 1, child.is_link);
    }
}

void Sidebar::set_places(std::vector<fs::Place> places) {
    sources_ = std::move(places);
    rebuild();
}

void Sidebar::rebuild() {
    if (rebuilding_)
        return;
    rebuilding_ = true;
    std::vector<fs::Place> places;
    if (!tree_) {
        for (const auto section : {fs::Place::Section::places, fs::Place::Section::bookmarks,
                                   fs::Place::Section::devices}) {
            if (std::ranges::find(collapsed_, std::to_string(static_cast<int>(section))) !=
                collapsed_.end())
                continue;
            std::vector<fs::Place> group;
            for (const auto& place : sources_) {
                if (place.section == section &&
                    std::ranges::find(hidden_, place_key(place)) == hidden_.end())
                    group.push_back(place);
            }
            const auto prefix = "separator:" + std::to_string(static_cast<int>(section)) + ":";
            for (const auto& key : order_) {
                if (key.starts_with(prefix)) {
                    places.push_back({.section = section,
                                      .label = std::string{i18n::tr("Separator")},
                                      .path = {},
                                      .icon = "minus",
                                      .icon_key = "sidebar_separator",
                                      .uri = key});
                } else {
                    auto it = std::ranges::find_if(
                        group, [&](const auto& p) { return place_key(p) == key; });
                    if (it != group.end()) {
                        places.push_back(*it);
                        group.erase(it);
                    }
                }
            }
            places.insert(places.end(), group.begin(), group.end());
        }
    }
    // The cursor stays on its place when that is still listed.
    std::optional<std::string> at;
    if (cursor_ && *cursor_ < places_.size()) {
        at = place_key(places_[*cursor_]);
    }
    places_ = std::move(places);
    depths_.clear();
    links_.clear();
    if (tree_)
        append_tree("/", 0);
    cursor_.reset();
    if (at) {
        const auto it =
            std::ranges::find_if(places_, [&](const auto& p) { return place_key(p) == *at; });
        if (it != places_.end()) {
            cursor_ = static_cast<std::size_t>(it - places_.begin());
        }
    }
    if (focused_ && !cursor_ && !places_.empty()) {
        cursor_ = 0;
    }
    if (renaming_ && std::ranges::none_of(places_, [&](const fs::Place& place) {
            return place.section == fs::Place::Section::bookmarks && place.path == *renaming_;
        })) {
        renaming_.reset();  // gone meanwhile (another program changed the bookmarks)
    }
    hovered_.reset();
    eject_rects_.clear();
    current_.reset();
    layout();
    if (tree_ && reveal_selection_) {
        set_cursor_to(tree_current_);
        scroller_.set_extent(content_height_, bounds_.height);
        for (const auto& row : rows_)
            if (row.place && row.place == cursor_) {
                if (row.top < scroller_.offset() + 38 ||
                    row.top + kRowHeight * scale() > scroller_.offset() + bounds_.height)
                    scroller_.jump_to(
                        std::max(0.0F, row.top - std::max(38.0F, bounds_.height / 2)));
            }
        reveal_selection_ = false;
    }
    rebuilding_ = false;
    redraw_();
}

std::optional<ui::Rect> Sidebar::place_icon_rect(const std::filesystem::path& path) const {
    if (bounds_.empty())
        return std::nullopt;
    for (const auto& row : rows_) {
        if (row.place && places_[*row.place].path == path) {
            const float y = bounds_.y + row.top - scroller_.offset();
            const float size = kRowHeight * scale();
            if (y >= bounds_.y && y + size <= bounds_.bottom())
                return ui::Rect{bounds_.x + kIconLeft * scale(), y, size, size};
        }
    }
    return std::nullopt;
}

void Sidebar::layout() {
    rows_.clear();
    float y = 38.0F + (kTop * scale());
    for (const auto section :
         {fs::Place::Section::places, fs::Place::Section::bookmarks, fs::Place::Section::devices}) {
        if (tree_ && section != fs::Place::Section::places)
            continue;
        if (!tree_ && std::ranges::none_of(
                          sources_, [section](const auto& p) { return p.section == section; }))
            continue;
        rows_.push_back({.top = y, .place = std::nullopt, .section = section});
        y += kSectionHeight * scale();
        for (std::size_t i = 0; i < places_.size(); ++i) {
            if (places_[i].section != section)
                continue;
            rows_.push_back({.top = y, .place = i, .section = section});
            y += kRowHeight * scale();
        }
        y += kSectionGap * scale();
    }
    content_height_ = y + kTop * scale();
}

float Sidebar::width_in(float window_width) const {
    const float room = window_width - kMinContent;
    return room < kMinWidth ? 0.0F : std::min(width_, room);
}

void Sidebar::set_width(float width) {
    width = std::clamp(width, kMinWidth, kMaxWidth);
    if (width != width_) {
        width_ = width;
        redraw_();
    }
}

void Sidebar::set_list_size(std::size_t index) {
    index = std::min(index, kListSizes.size() - 1);
    if (index == list_size_) {
        return;
    }
    const float old_scale = scale();
    const float offset = scroller_.offset();
    list_size_ = index;
    layout();
    scroller_.set_extent(content_height_, bounds_.height);
    scroller_.jump_to(offset * scale() / old_scale);
    eject_rects_.clear();
    update_hover();
    redraw_();
}

void Sidebar::set_focused(bool focused) {
    if (focused == focused_) {
        return;
    }
    focused_ = focused;
    if (focused_) {
        cursor_ = current_;
        if (!cursor_ && !places_.empty()) {
            cursor_ = 0;
        }
    }
    redraw_();
}

void Sidebar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons, const std::filesystem::path& current,
                   Clock::time_point now) {
    bounds_ = bounds;
    current_ = place_holding(places_, current);
    renderer.fill_background(bounds, palette.surface_container_low);
    scroller_.set_extent(content_height_, bounds.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();

    renderer.push_clip({bounds.x, bounds.y + 38, bounds.width, std::max(0.0F, bounds.height - 38)});
    eject_rects_.assign(places_.size(), ui::Rect{});
    for (const Row& row : rows_) {
        const float y = bounds.y + row.top - offset;
        if (y + (kRowHeight * scale()) < bounds.y || y > bounds.bottom()) {
            continue;
        }
        if (!row.place) {
            const bool collapsed =
                std::ranges::find(collapsed_, std::to_string(static_cast<int>(row.section))) !=
                collapsed_.end();
            icons.draw(renderer, collapsed && !tree_ ? "chevron-right" : "chevron-down",
                       bounds.x + 6, y + 10 * scale(), 12, palette.on_surface_variant);
            renderer.draw_text(tree_ ? i18n::tr("FOLDERS") : title_of(row.section),
                               bounds.x + 24 * scale(), y + 14.0F * scale(),
                               (kSectionSize * scale()), palette.on_surface_variant);
            continue;
        }
        const std::size_t i = *row.place;
        const fs::Place& place = places_[i];
        const float indent = tree_ ? std::min(static_cast<float>(depths_[i]) * 14.0F,
                                              std::max(0.0F, bounds.width - 100.0F)) +
                                         14.0F
                                   : 0.0F;
        if (separator(place)) {
            renderer.fill_rect(
                {bounds.x + kInset, y + kRowHeight * scale() / 2, bounds.width - 2 * kInset, 1},
                focused_ && cursor_ == i ? palette.primary : palette.outline_variant);
            continue;
        }
        // The label being edited: its field on a quiet row, not on the cursor's colour.
        const bool editing =
            renaming_ && place.section == fs::Place::Section::bookmarks && place.path == *renaming_;
        const bool is_cursor = focused_ && cursor_ == i && !editing;
        const bool is_current = current_ == i;
        std::optional<ui::Color> fill;
        // A disk not mounted is quieter until a click mounts it.
        ui::Color icon = place.disk.mounted ? palette.primary : palette.on_surface_variant;
        ui::Color text = place.disk.mounted ? palette.on_surface : palette.on_surface_variant;
        if (drop_target_ == place.path) {
            fill = palette.primary_container;
            icon = palette.on_primary_container;
            text = palette.on_primary_container;
        } else if (is_cursor) {
            fill = palette.primary;
            icon = palette.on_primary;
            text = palette.on_primary;
        } else if (is_current) {
            fill = palette.surface_container_highest;
        } else if (hovered_ == i) {
            fill = palette.surface_container_high;
        }
        if (place.disk.mounted && !is_cursor && drop_target_ != place.path)
            icon = theme::readable_foreground(palette.primary,
                                              fill.value_or(palette.surface_container_low));
        if (fill) {
            const bool drop = drop_target_ == place.path;
            renderer.draw_box({bounds.x + kInset, y + 1.0F, bounds.width - (2.0F * kInset),
                               (kRowHeight * scale()) - 2.0F},
                              {.fill = *fill,
                               .radius = {},
                               .border_width = drop ? 1.5F : 0.0F,
                               .border = palette.primary});
        }
        if (tree_ && !links_[i])
            icons.draw(renderer, models_.contains(place.path) ? "chevron-down" : "chevron-right",
                       bounds.x + indent - 8, y + 8 * scale(), 12, text);
        const auto icon_key = place.icon_key == "trash" && place.icon == "trash-filled"
                                  ? std::string_view{"trash_full"}
                                  : place.icon_key;
        const float icon_x = bounds.x + (kIconLeft * scale()) + indent;
        const float icon_y = y + (((kRowHeight * scale()) - (kIconSize * scale())) / 2.0F);
        if (place.section == fs::Place::Section::bookmarks && place.uri.empty()) {
            icons.draw_file(renderer, "inode/directory", place.icon, icon_x, icon_y,
                            (kIconSize * scale()), icon, place.folder_icon);
        } else {
            icons.draw_place(renderer, icon_key, place.icon, icon_x, icon_y, (kIconSize * scale()),
                             icon);
        }
        float text_room = bounds.width - (kTextLeft * scale()) - kInset - 4.0F - indent;
        if (place.disk.ejectable) {
            const ui::Rect eject{bounds.right() - kInset - 6.0F - (kEjectSize * scale()),
                                 y + (((kRowHeight * scale()) - (kEjectSize * scale())) / 2.0F),
                                 (kEjectSize * scale()), (kEjectSize * scale())};
            eject_rects_[i] = eject;
            icons.draw(renderer, "player-eject", eject.x, eject.y, (kEjectSize * scale()),
                       hovered_eject_ == i ? icon : text.with_alpha(0.7F));
            text_room -= (kEjectSize * scale()) + 8.0F;
        }
        if (editing) {
            field_.draw(renderer,
                        {bounds.x + kTextLeft * scale() - 6.0F, y + 3.0F,
                         std::max(1.0F, text_room + 6.0F), kRowHeight * scale() - 6.0F},
                        kTextSize * scale(),
                        {.background = palette.surface_container_highest,
                         .border = palette.primary,
                         .text = palette.on_surface,
                         .selection = palette.secondary_container,
                         .caret = palette.on_surface});
            continue;
        }
        std::string label = place.label;
        if (tree_) {
            const auto model = models_.find(place.path);
            if (model != models_.end()) {
                if (model->second->loading())
                    label += " …";
                else if (!model->second->problem().empty())
                    label += " — " + model->second->problem();
            }
        }
        renderer.draw_text(label, bounds.x + (kTextLeft * scale()) + indent, y + 8.0F * scale(),
                           (kTextSize * scale()), text, std::max(1.0F, text_room),
                           ui::Ellipsis::middle);
    }
    if (moving_separator_ && hovered_ && places_[*hovered_].section == moving_separator_->section) {
        for (const auto& row : rows_)
            if (row.place == hovered_)
                renderer.fill_rect(
                    {bounds.x + kInset, bounds.y + row.top - offset, bounds.width - 2 * kInset, 2},
                    palette.primary);
    }
    if (bookmark_drop_) {
        // After the last bookmark, or where the first one would go: after the places.
        float line = (kTop * scale());
        for (const Row& row : rows_) {
            if (row.place && row.section != fs::Place::Section::devices) {
                line = row.top + (kRowHeight * scale());
            }
        }
        renderer.draw_box({bounds.x + kInset, bounds.y + line - offset - 1.0F,
                           bounds.width - (2.0F * kInset), 2.0F},
                          {.fill = palette.primary, .radius = {}});
    }
    renderer.pop_clip();
    renderer.fill_background({bounds.x, bounds.y, bounds.width, 38}, palette.surface_container_low);
    close_bounds_ = {bounds.right() - 30, bounds.y + 4, 26, 30};
    const float tab_width = std::max(1.0F, (bounds.width - 38) / 2);
    places_bounds_ = {bounds.x + 4, bounds.y + 4, tab_width, 30};
    tree_bounds_ = {places_bounds_.right(), bounds.y + 4, tab_width, 30};
    const auto tab = [&](const ui::Rect& rect, std::string_view label, bool selected) {
        if (selected)
            renderer.draw_box(rect, {.fill = palette.primary_container, .radius = {}});
        renderer.draw_text(label, rect.x + 6, rect.y + 8, 12,
                           selected ? palette.on_primary_container : palette.on_surface,
                           rect.width - 12);
    };
    tab(places_bounds_, i18n::tr("Places"), !tree_);
    tab(tree_bounds_, i18n::tr("Tree"), tree_);
    icons.draw(renderer, "x", close_bounds_.x + 5, close_bounds_.y + 7, 16, palette.on_surface);

    const bool grabbed = drag_ || over_edge_;
    renderer.fill_rect({bounds.right() - (grabbed ? 1.0F : 1.0F / renderer.scale()), bounds.y,
                        grabbed ? 2.0F : 1.0F / renderer.scale(), bounds.height},
                       grabbed ? palette.primary : palette.outline_variant);
    update_hover();  // content may have moved under a still pointer
}

const fs::Place* Sidebar::place_under(float x, float y) const {
    const std::optional<std::size_t> place = place_at(x, y);
    return place ? &places_[*place] : nullptr;
}

void Sidebar::set_drop_target(std::optional<std::filesystem::path> path) {
    if (path != drop_target_) {
        drop_target_ = std::move(path);
        redraw_();
    }
}

bool Sidebar::in_bookmark_zone(float x, float y) const {
    if (tree_ || bounds_.empty() || x < bounds_.x || x >= bounds_.right() - kEdgeGrab ||
        y < bounds_.y + 38 || y >= bounds_.bottom()) {
        return false;
    }
    const float content_y = y - bounds_.y + scroller_.offset();
    if (content_y >= content_height_ - (kTop * scale())) {
        return true;  // below the last place
    }
    return std::ranges::any_of(rows_, [this, content_y](const Row& row) {
        return !row.place && row.section == fs::Place::Section::bookmarks && content_y >= row.top &&
               content_y < row.top + (kSectionHeight * scale());
    });
}

void Sidebar::set_bookmark_drop(bool shown) {
    if (shown != bookmark_drop_) {
        bookmark_drop_ = shown;
        redraw_();
    }
}

std::optional<std::size_t> Sidebar::eject_at(float x, float y) const {
    const std::optional<std::size_t> place = place_at(x, y);
    if (place && *place < eject_rects_.size() && !eject_rects_[*place].empty() &&
        x >= eject_rects_[*place].x - 4.0F && x < eject_rects_[*place].right() + 4.0F) {
        return place;
    }
    return std::nullopt;
}

bool Sidebar::clickable_at(float x, float y) const {
    return close_bounds_.contains(x, y) || places_bounds_.contains(x, y) ||
           tree_bounds_.contains(x, y) || eject_at(x, y).has_value();
}

std::optional<std::size_t> Sidebar::place_at(float x, float y) const {
    if (bounds_.empty() || x < bounds_.x || x >= bounds_.right() || y < bounds_.y ||
        y >= bounds_.bottom() || y < bounds_.y + 38) {
        return std::nullopt;
    }
    const float content_y = y - bounds_.y + scroller_.offset();
    for (const Row& row : rows_) {
        if (row.place && content_y >= row.top && content_y < row.top + (kRowHeight * scale())) {
            return row.place;
        }
    }
    return std::nullopt;
}

bool Sidebar::on_edge(float x, float y) const {
    return !bounds_.empty() && std::abs(x - bounds_.right()) <= kEdgeGrab && y >= bounds_.y &&
           y < bounds_.bottom();
}

bool Sidebar::claims(float x, float y) const {
    if (drag_ || moving_separator_) {
        return true;
    }
    if (bounds_.empty() || y < bounds_.y || y >= bounds_.bottom()) {
        return false;
    }
    return (x >= bounds_.x && x < bounds_.right()) || on_edge(x, y);
}

void Sidebar::update_hover() {
    const std::optional<std::size_t> place =
        pointer_inside_ && !drag_ && !on_edge(pointer_x_, pointer_y_)
            ? place_at(pointer_x_, pointer_y_)
            : std::nullopt;
    if (place != hovered_) {
        hovered_ = place;
        redraw_();
    }
    const std::optional<std::size_t> eject =
        place ? eject_at(pointer_x_, pointer_y_) : std::nullopt;
    if (eject != hovered_eject_) {
        hovered_eject_ = eject;
        redraw_();
    }
    const bool over = drag_ || (pointer_inside_ && on_edge(pointer_x_, pointer_y_));
    if (over != over_edge_) {
        over_edge_ = over;
        set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
        redraw_();
    }
}

void Sidebar::choose(std::size_t place) {
    const fs::Place chosen = places_[place];  // open_ may replace the places
    if (separator(chosen))
        return;
    focused_ = false;
    redraw_();
    if (open_) {
        open_(chosen);
    }
}

std::optional<ui::Rect> Sidebar::eject_icon(const std::filesystem::path& path) const {
    for (std::size_t i = 0; i < places_.size() && i < eject_rects_.size(); ++i) {
        if (places_[i].path == path && !eject_rects_[i].empty()) {
            return eject_rects_[i];
        }
    }
    return std::nullopt;
}

void Sidebar::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion:
            pointer_inside_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            if (drag_) {
                set_width(drag_->start_width + (event.x - drag_->start_x));
            }
            update_hover();
            break;
        case Kind::leave:
            pointer_inside_ = false;
            update_hover();
            break;
        case Kind::scroll:
            if (event.modifiers.ctrl && event.wheel_y != 0.0F) {
                zoom_wheel_ += event.wheel_y;
                while (zoom_wheel_ <= -1.0F) {
                    zoom_wheel_ += 1.0F;
                    set_list_size(list_size_ + 1);
                }
                while (zoom_wheel_ >= 1.0F) {
                    zoom_wheel_ -= 1.0F;
                    set_list_size(list_size_ > 0 ? list_size_ - 1 : 0);
                }
                break;
            }
            zoom_wheel_ = 0.0F;
            if (event.wheel_y != 0.0F) {
                scroller_.wheel(event.wheel_y, now);
            } else if (event.scroll_y != 0.0F) {
                scroller_.finger(event.scroll_y, now);
            }
            if (event.scroll_finger && event.scroll_stop) {
                scroller_.release(now);
            }
            redraw_();
            break;
        case Kind::press: {
            if (event.button == ui::PointerButton::right) {
                if (const std::optional<std::size_t> place = place_at(event.x, event.y);
                    place && menu_) {
                    const fs::Place chosen = places_[*place];  // menu_ may replace the places
                    menu_(chosen, event.x, event.y);
                } else if (background_menu_)
                    background_menu_(event.x, event.y);
                break;
            }
            if (event.button == ui::PointerButton::left && !on_edge(event.x, event.y)) {
                if (close_bounds_.contains(event.x, event.y)) {
                    if (close_)
                        close_();
                    return;
                }
                if (places_bounds_.contains(event.x, event.y)) {
                    set_tree(false);
                    return;
                }
                if (tree_bounds_.contains(event.x, event.y)) {
                    set_tree(true);
                    return;
                }
                const float y = event.y - bounds_.y + scroller_.offset();
                for (const auto& row : rows_) {
                    if (!row.place && !tree_ && event.y >= bounds_.y + 38 && y >= row.top &&
                        y < row.top + kSectionHeight * scale()) {
                        toggle_section(row.section);
                        return;
                    }
                }
            }
            if (event.button != ui::PointerButton::left) {
                break;
            }
            if (on_edge(event.x, event.y)) {
                if (event.click_count == 2) {
                    set_width(kWidth);
                } else {
                    drag_ = Drag{.start_x = event.x, .start_width = width_};
                }
                update_hover();
                break;
            }
            if (const std::optional<std::size_t> place = place_at(event.x, event.y)) {
                if (separator(places_[*place])) {
                    moving_separator_ = places_[*place];
                    cursor_ = place;
                    focused_ = true;
                    redraw_();
                    return;
                }
                if (tree_) {
                    const float indent = std::min(static_cast<float>(depths_[*place]) * 14.0F,
                                                  std::max(0.0F, bounds_.width - 100.0F)) +
                                         14;
                    if (event.x < bounds_.x + indent + 8) {
                        toggle_folder(places_[*place].path);
                        return;
                    }
                }
                const bool on_eject = *place < eject_rects_.size() &&
                                      !eject_rects_[*place].empty() &&
                                      event.x >= eject_rects_[*place].x - 4.0F &&
                                      event.x < eject_rects_[*place].right() + 4.0F;
                if (on_eject && eject_) {
                    const fs::Place chosen = places_[*place];  // eject_ may replace the places
                    eject_(chosen);
                } else {
                    choose(*place);
                }
            }
            break;
        }
        case Kind::release:
            if (moving_separator_ && event.button == ui::PointerButton::left) {
                const auto target = place_at(event.x, event.y);
                if (target && places_[*target].section == moving_separator_->section &&
                    place_key(places_[*target]) != place_key(*moving_separator_)) {
                    const auto before = place_key(places_[*target]);
                    const auto moved = place_key(*moving_separator_);
                    std::erase(order_, moved);
                    order_.insert(std::ranges::find(order_, before), moved);
                }
                moving_separator_.reset();
                rebuild();
            }
            if (drag_ && event.button == ui::PointerButton::left) {
                drag_.reset();
                update_hover();
            }
            break;
    }
}

void Sidebar::start_rename(const std::filesystem::path& folder) {
    const auto it = std::ranges::find_if(places_, [&](const fs::Place& place) {
        return place.section == fs::Place::Section::bookmarks && place.uri.empty() &&
               place.path == folder;
    });
    if (it == places_.end()) {
        return;
    }
    renaming_ = folder;
    renaming_from_ = it->label;
    field_.set_text(it->label);
    field_.select_all();
    redraw_();
}

bool Sidebar::rename_key(const ui::KeyEvent& event) {
    if (!renaming_) {
        return false;
    }
    const bool plain = !event.modifiers.ctrl && !event.modifiers.alt;
    if (plain && (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter)) {
        confirm_rename();
    } else if (plain && event.keysym == XKB_KEY_Escape) {
        cancel_rename();
    } else if (field_.key(event)) {
        redraw_();
    }
    return true;
}

bool Sidebar::rename_pointer(const ui::PointerEvent& event) {
    if (!renaming_) {
        return false;
    }
    if (field_.pointer(event)) {
        redraw_();
        return true;
    }
    if (event.kind != ui::PointerEvent::Kind::press) {
        return false;
    }
    confirm_rename();
    return false;
}

void Sidebar::paste(std::string_view text) {
    if (!renaming_) {
        return;
    }
    std::string line;
    for (const char c : text.substr(0, text.find_first_of("\r\n"))) {
        if (static_cast<unsigned char>(c) >= 0x20U && c != 0x7F) {
            line.push_back(c);
        }
    }
    field_.insert(line);
    redraw_();
}

std::string Sidebar::cut() {
    if (!renaming_) {
        return {};
    }
    std::string text = field_.cut();
    if (!text.empty()) {
        redraw_();
    }
    return text;
}

void Sidebar::confirm_rename() {
    if (!renaming_) {
        return;
    }
    const std::filesystem::path folder = *renaming_;
    const std::string label = field_.text();
    renaming_.reset();
    redraw_();
    if (label != renaming_from_ && rename_) {
        rename_(folder, label);
    }
}

void Sidebar::cancel_rename() {
    if (renaming_) {
        renaming_.reset();
        redraw_();
    }
}

bool Sidebar::edit_key(const ui::KeyEvent& event) {
    const bool rename = event.keysym == XKB_KEY_F2 && event.modifiers == ui::Modifiers{};
    const bool move = (event.keysym == XKB_KEY_Up || event.keysym == XKB_KEY_Down) &&
                      event.modifiers == ui::Modifiers{.alt = true};
    if (!focused_ || (!rename && !move)) {
        return false;
    }
    if (move && cursor_ && *cursor_ < places_.size() && !tree_) {
        move_place(places_[*cursor_], event.keysym == XKB_KEY_Up ? -1 : 1);
        return true;
    }
    if (tree_)
        return true;
    if (!cursor_ || *cursor_ >= places_.size() ||
        (places_[*cursor_].section != fs::Place::Section::bookmarks ||
         !places_[*cursor_].uri.empty())) {
        return true;  // nothing to edit here, and nothing for the view either
    }
    const std::filesystem::path folder = places_[*cursor_].path;
    if (rename) {
        start_rename(folder);
    }
    return true;
}

void Sidebar::set_cursor_to(const std::filesystem::path& path) {
    const auto it = std::ranges::find(places_, path, &fs::Place::path);
    if (it != places_.end()) {
        cursor_ = static_cast<std::size_t>(it - places_.begin());
        redraw_();
    }
}

std::pair<float, float> Sidebar::cursor_anchor() const {
    for (const auto& row : rows_) {
        if (row.place == cursor_)
            return {bounds_.x + 24,
                    std::clamp(bounds_.y + row.top - scroller_.offset() + kRowHeight * scale(),
                               bounds_.y, bounds_.bottom())};
    }
    return {bounds_.x + 24, bounds_.y + 32};
}

bool Sidebar::key(const ui::KeyEvent& event) {
    if (event.modifiers.ctrl && !event.modifiers.alt && !event.modifiers.super) {
        switch (event.keysym) {
            case XKB_KEY_plus:
            case XKB_KEY_equal:
            case XKB_KEY_KP_Add:
                set_list_size(list_size_ + 1);
                return true;
            case XKB_KEY_minus:
            case XKB_KEY_KP_Subtract:
                set_list_size(list_size_ > 0 ? list_size_ - 1 : 0);
                return true;
            case XKB_KEY_0:
            case XKB_KEY_KP_0:
                set_list_size(kUsualListSize);
                return true;
            default:
                break;
        }
    }
    if (places_.empty()) {
        if (event.keysym == XKB_KEY_Escape) {
            set_focused(false);
            return true;
        }
        return false;
    }
    const std::size_t last = places_.size() - 1;
    const std::size_t at = cursor_.value_or(0);
    const auto page =
        static_cast<std::size_t>(std::max(1.0F, bounds_.height / (kRowHeight * scale()) - 1));
    switch (event.keysym) {
        case XKB_KEY_Page_Up:
            cursor_ = at > page ? at - page : 0;
            break;
        case XKB_KEY_Page_Down:
            cursor_ = std::min(at + page, last);
            break;
        case XKB_KEY_Up:
            cursor_ = at > 0 ? at - 1 : 0;
            break;
        case XKB_KEY_Down:
            cursor_ = std::min(at + 1, last);
            break;
        case XKB_KEY_Home:
            cursor_ = 0;
            break;
        case XKB_KEY_End:
            cursor_ = last;
            break;
        case XKB_KEY_Right:
            if (!tree_)
                return true;
            if (!models_.contains(places_[at].path))
                toggle_folder(places_[at].path);
            else if (at < last && depths_[at + 1] > depths_[at])
                cursor_ = at + 1;
            break;
        case XKB_KEY_Left:
            if (!tree_)
                return true;
            if (models_.contains(places_[at].path))
                toggle_folder(places_[at].path);
            else
                set_cursor_to(places_[at].path.parent_path());
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(at);
            return true;
        case XKB_KEY_Escape:
            set_focused(false);
            return true;
        default:
            return false;
    }
    // Keep the cursor's row in sight.
    for (const Row& row : rows_) {
        if (row.place == cursor_) {
            const float from = scroller_.destination();
            if (row.top < from + 38) {
                scroller_.glide_to(row.top - 38 - (kTop * scale()), Clock::now());
            } else if (row.top + (kRowHeight * scale()) > from + bounds_.height) {
                scroller_.glide_to(
                    row.top + (kRowHeight * scale()) + (kTop * scale()) - bounds_.height,
                    Clock::now());
            }
            break;
        }
    }
    redraw_();
    return true;
}

void Sidebar::draw_drive_tooltip(ui::Renderer& renderer, const theme::Palette& palette,
                                 const ui::Rect& window, std::string_view text) const {
    const auto* place = hovered_place();
    if (!place || text.empty())
        return;
    const float width = std::min(380.0F, std::max(1.0F, window.width - 16));
    const ui::Rect box{
        std::clamp(bounds_.right() - 12, window.x + 8,
                   std::max(window.x + 8, window.right() - width - 8)),
        std::clamp(pointer_y_ + 18, window.y + 8, std::max(window.y + 8, window.bottom() - 64)),
        width, 56};
    renderer.draw_box(box, {.fill = palette.surface_container_high,
                            .radius = {},
                            .border_width = 1,
                            .border = palette.outline_variant});
    renderer.draw_text(place->path.string(), box.x + 8, box.y + 7, 12, palette.on_surface,
                       width - 16, ui::Ellipsis::middle);
    renderer.draw_text(text, box.x + 8, box.y + 31, 12, palette.on_surface_variant, width - 16);
}

}  // namespace rummage::views
