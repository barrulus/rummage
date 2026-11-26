#include "rummage/views/column.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "draw_helpers.hpp"
#include "item_style.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

constexpr float kRowInset = 6.0F;  // highlight box inside the column
constexpr float kIconX = 14.0F;
constexpr float kIconGap = 10.0F;
constexpr float kChevronSize = 14.0F;
constexpr float kRightPadding = 12.0F;
constexpr float kItemSlack = 8.0F;  // past the end of a name, still the entry (a press drags it)

}  // namespace

Column::Column(std::shared_ptr<Folder> folder) : folder_{std::move(folder)} {}

void Column::set_size(const ListSize& size) {
    if (size == size_) {
        return;
    }
    const float offset = scroller_.offset();
    const std::optional<std::size_t> cursor = folder_->cursor();
    const float old_row = size_.row;
    size_ = size;
    if (cursor) {
        // The same height on screen, the whole row still in sight.
        const float screen = std::clamp((static_cast<float>(*cursor) * old_row) - offset, 0.0F,
                                        std::max(0.0F, rows_area_.height - size_.row));
        scroller_.jump_to(std::max(0.0F, (static_cast<float>(*cursor) * size_.row) - screen));
    } else {
        scroller_.jump_to(offset * size_.row / old_row);
    }
}

std::size_t Column::page_rows() const {
    const auto rows = static_cast<std::size_t>(rows_area_.height / size_.row);
    return std::max<std::size_t>(1, rows > 1 ? rows - 1 : rows);
}

void Column::reveal_cursor(Folder::Reveal reveal, Clock::time_point now) {
    const std::optional<std::size_t> cursor = folder_->cursor();
    const float viewport = rows_area_.height;
    if (reveal == Folder::Reveal::none || !cursor) {
        return;
    }
    if (viewport <= 0.0F) {
        folder_->request_jump();  // nowhere to show it yet
        return;
    }
    reveal_row(scroller_, *cursor, size_.row, viewport, reveal == Folder::Reveal::jump, now);
}

void Column::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                  ui::IconSet& icons, bool focused, Clock::time_point now, EditSlot* edit,
                  const CutMarks* cut, const DropMark* drop, bool show_extensions,
                  Thumbnails* thumbnails) {
    motion_bounds_ = {};
    const std::vector<fs::Entry>& entries = folder_->entries();
    rows_area_ = bounds;
    const float content = static_cast<float>(entries.size()) * size_.row;
    scroller_.set_extent(content, bounds.height);
    reveal_cursor(folder_->take_reveal(), now);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    const RowRange range = visible_rows(entries.size(), size_.row, offset, bounds.height);

    renderer.push_clip(bounds);
    const float name_x = bounds.x + kIconX + size_.icon + kIconGap;
    item_first_ = range.first;
    item_right_.clear();
    const std::optional<std::size_t> cursor = folder_->cursor();
    const bool cursor_ring = folder_->single_selection() == nullptr;
    for (std::size_t i = range.first; i < range.last; ++i) {
        const fs::Entry& entry = entries[i];
        const float y = bounds.y + (static_cast<float>(i) * size_.row) - offset;
        motion_bounds_.include(ui::Rect{bounds.x + kRowInset, y + 1.0F,
                                        bounds.width - 2.0F * kRowInset, size_.row - 2.0F},
                               bounds, cursor == i, folder_->is_selected(entry.key()));
        const ItemStyle style = item_style(palette, entry,
                                           {.selected = folder_->is_selected(entry.key()),
                                            .cursor = cursor == i,
                                            .ring = cursor_ring,
                                            .focused = focused,
                                            .hovered = hovered_ == i,
                                            .cut = cut != nullptr && cut->holds(path(), entry),
                                            .drop = drop != nullptr && drop->holds(path(), entry)});
        if (style.fill || style.ring) {
            renderer.draw_box({bounds.x + kRowInset, y + 1.0F, bounds.width - (2.0F * kRowInset),
                               size_.row - 2.0F},
                              {.fill = style.fill.value_or(kTransparent),
                               .radius = {},
                               .border_width = style.ring ? 1.5F : 0.0F,
                               .border = style.ring.value_or(kTransparent)});
        }
        draw_entry_icon(
            renderer, icons, palette, *folder_, entry, style, thumbnails,
            {bounds.x + kIconX, y + ((size_.row - size_.icon) / 2.0F), size_.icon, size_.icon},
            cut != nullptr && cut->holds(path(), entry),
            cursor == i ? &motion_bounds_.visual : nullptr);
        if (cursor == i)
            motion_bounds_.visual.clip = motion_bounds_.visual.bounds.intersect(bounds);
        float name_right = bounds.right() - kRightPadding;
        if (entry.is_directory() && !folder_->is_external_collection()) {
            name_right -= kChevronSize + 6.0F;
            icons.draw(renderer, "chevron-right", bounds.right() - kRightPadding - kChevronSize,
                       y + ((size_.row - kChevronSize) / 2.0F), kChevronSize, style.chevron);
        }
        if (entry.decoration && name_right - name_x >= kStatusWidth + 16) {
            draw_entry_status(renderer, palette, entry,
                              {name_right - kStatusWidth, y, kStatusWidth, size_.row}, style.text);
            name_right -= kStatusWidth;
        }
        if (edit != nullptr && edit->holds(*folder_, entry)) {
            edit->place({name_x - ui::TextField::kPadding, y + 3.0F,
                         std::max(1.0F, name_right - name_x) + (2.0F * ui::TextField::kPadding),
                         size_.row - 6.0F},
                        size_.text);
            item_right_.push_back(name_right);
            continue;
        }
        const float drawn =
            renderer.draw_text(display_name(entry.label(), entry.is_directory(), show_extensions),
                               name_x, y + ((size_.row - size_.text) / 2.0F) - 2.0F, size_.text,
                               style.text, std::max(1.0F, name_right - name_x), ui::Ellipsis::end);
        item_right_.push_back(name_x + drawn + kItemSlack);
    }

    const fs::DirectoryModel& model = folder_->model();
    if (model.error()) {
        draw_centered(renderer, model.problem(), bounds, bounds.y + 24.0F, size_.text,
                      palette.error);
    } else if (entries.empty() && !folder_->loading()) {
        draw_centered(renderer,
                      folder_->is_external_collection()
                          ? i18n::tr("No results")
                          : i18n::tr(folder_->is_search() ? "Find Files…" : "Empty folder"),
                      bounds, bounds.y + 24.0F, size_.text, palette.on_surface_variant);
    }

    if (drop != nullptr && drop->covers(path())) {
        renderer.draw_box(
            {bounds.x + 2.0F, bounds.y + 2.0F, bounds.width - 4.0F, bounds.height - 4.0F},
            {.fill = kTransparent, .radius = {}, .border_width = 2.0F, .border = palette.primary});
    }

    scrollbar_.layout(bounds);
    if (const ui::Rect thumb = scrollbar_.thumb(scroller_); !thumb.empty()) {
        renderer.draw_box(
            thumb,
            {.fill = palette.on_surface_variant.with_alpha(scrollbar_.dragging() ? 0.75F : 0.45F),
             .radius = {}});
    }
    renderer.pop_clip();

    folder_->sniff(range.first, range.last);
}

std::optional<std::size_t> Column::row_at(float x, float y) const {
    if (x < rows_area_.x || x >= rows_area_.right() || y < rows_area_.y ||
        y >= rows_area_.bottom()) {
        return std::nullopt;
    }
    const float content_y = y - rows_area_.y + scroller_.offset();
    const auto row = static_cast<std::size_t>(content_y / size_.row);
    if (row >= folder_->entries().size()) {
        return std::nullopt;
    }
    return row;
}

bool Column::on_item(std::size_t row, float x) const {
    if (row < item_first_ || row - item_first_ >= item_right_.size()) {
        return false;
    }
    return x >= rows_area_.x + kRowInset && x < item_right_[row - item_first_];
}

std::vector<std::size_t> Column::rows_in(const ui::Rect& content) const {
    return rows_touched(content.y, content.bottom(), size_.row, folder_->entries().size());
}

bool Column::set_hovered(std::optional<std::size_t> row) {
    if (row == hovered_) {
        return false;
    }
    hovered_ = row;
    return true;
}

std::optional<std::pair<float, float>> Column::cursor_anchor() const {
    const std::optional<std::size_t> cursor = folder_->cursor();
    if (!cursor) {
        return std::nullopt;
    }
    const float bottom =
        rows_area_.y + (static_cast<float>(*cursor + 1) * size_.row) - scroller_.offset();
    if (bottom - size_.row < rows_area_.y || bottom > rows_area_.bottom()) {
        return std::nullopt;
    }
    return std::pair{rows_area_.x + 32.0F, bottom};
}

}  // namespace rummage::views
