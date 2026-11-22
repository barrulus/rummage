#include "rummage/views/batch_rename_dialog.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {
constexpr float kRow = 24.0F;
constexpr std::size_t kPreview = 6;
constexpr std::size_t kCancel = 7;
constexpr std::size_t kApply = 8;
constexpr std::array<std::string_view, 6> labels{
    "Find in name (literal)", "Replace with", "Template", "Start", "Step", "Zero padding"};

bool number(std::string_view text, std::uint64_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}  // namespace

BatchRenameDialog::BatchRenameDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void BatchRenameDialog::open(std::vector<fs::RenameSource> directory,
                             std::vector<std::string> selection, Apply apply) {
    busy_ = false;
    ++generation_;
    directory_ = std::move(directory);
    selection_ = std::move(selection);
    apply_ = std::move(apply);
    for (auto& field : fields_) {
        field.set_text({});
    }
    fields_[2].set_text("{name}{ext}");
    fields_[3].set_text("1");
    fields_[4].set_text("1");
    fields_[5].set_text("2");
    focus_ = 0;
    offset_ = 0;
    form_offset_ = 0;
    update();
}

void BatchRenameDialog::open_busy(std::string message) {
    busy_ = true;
    busy_message_ = std::move(message);
    apply_ = nullptr;
    redraw_();
}

void BatchRenameDialog::close() {
    busy_ = false;
    apply_ = nullptr;
    directory_.clear();
    selection_.clear();
    redraw_();
}

void BatchRenameDialog::update() {
    fs::RenameOptions options{
        .find = fields_[0].text(), .replace = fields_[1].text(), .pattern = fields_[2].text()};
    std::uint64_t padding = 0;
    if (!number(fields_[3].text(), options.start) || !number(fields_[4].text(), options.step) ||
        !number(fields_[5].text(), padding) || padding > 20 || options.step == 0) {
        preview_ = {};
        preview_.error =
            std::string{i18n::tr("Start and step must be whole numbers; step > 0, padding 0–20")};
    } else {
        options.padding = static_cast<unsigned>(padding);
        preview_ = fs::plan_rename(directory_, selection_, options);
    }
    redraw_();
}

void BatchRenameDialog::submit() {
    if (apply_ && can_apply()) {
        const auto apply = apply_;
        const auto changes = preview_.changes;
        close();
        apply(changes);
    }
}

void BatchRenameDialog::scroll(float delta) {
    offset_ = std::clamp(offset_ + delta, 0.0F, max_scroll_);
    redraw_();
}

bool BatchRenameDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    if (busy_) {
        return true;
    }
    switch (event.keysym) {
        case XKB_KEY_Escape:
            close();
            return true;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            focus_ = (focus_ +
                      ((event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab) ? 8 : 1)) %
                     9;
            if (focus_ < fields_.size()) {
                fields_[focus_].select_all();
                const auto& rect = fields_[focus_].bounds();
                if (rect.y - 18.0F < body_.y) {
                    form_offset_ = std::max(0.0F, form_offset_ + rect.y - 18.0F - body_.y);
                } else if (rect.bottom() > body_.bottom()) {
                    form_offset_ =
                        std::min(max_form_scroll_, form_offset_ + rect.bottom() - body_.bottom());
                }
            } else if (focus_ == kPreview) {
                form_offset_ = max_form_scroll_;
            }
            redraw_();
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (focus_ == kCancel) {
                close();
            } else {
                submit();
            }
            return true;
        case XKB_KEY_Page_Down:
            focus_ = kPreview;
            if (form_offset_ < max_form_scroll_) {
                form_offset_ = max_form_scroll_;
                redraw_();
            } else {
                scroll(list_.height);
            }
            return true;
        case XKB_KEY_Page_Up:
            focus_ = kPreview;
            scroll(-list_.height);
            return true;
        default:
            break;
    }
    if (focus_ < fields_.size()) {
        if (fields_[focus_].key(event)) {
            update();
        }
    } else if (focus_ == kPreview) {
        if (event.keysym == XKB_KEY_Up) {
            scroll(-kRow);
        } else if (event.keysym == XKB_KEY_Down) {
            scroll(kRow);
        } else if (event.keysym == XKB_KEY_Home) {
            scroll(-max_scroll_);
        } else if (event.keysym == XKB_KEY_End) {
            scroll(max_scroll_);
        }
    } else if (event.keysym == XKB_KEY_space) {
        if (focus_ == kCancel) {
            close();
        } else {
            submit();
        }
    }
    return true;
}

std::string BatchRenameDialog::copy_selection(bool cut) {
    if (!is_open() || busy_ || focus_ >= fields_.size())
        return {};
    auto& field = fields_[focus_];
    auto text = cut ? field.cut() : field.selected_text();
    if (cut && !text.empty()) {
        update();
    }
    return text;
}

void BatchRenameDialog::paste(std::string_view text) {
    if (is_open() && !busy_ && focus_ < fields_.size()) {
        fields_[focus_].insert(text.substr(0, text.find_first_of("\r\n")));
        update();
    }
}

ui::CursorShape BatchRenameDialog::cursor_at(float x, float y) const {
    if (busy_) {
        return ui::CursorShape::progress;
    }
    for (const auto& field : fields_) {
        if (body_.contains(x, y) && field.bounds().contains(x, y)) {
            return ui::CursorShape::text;
        }
    }
    if (cancel_.contains(x, y) || (can_apply() && apply_button_.contains(x, y))) {
        return ui::CursorShape::pointer;
    }
    return ui::CursorShape::standard;
}

void BatchRenameDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open() || busy_) {
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press && focus_ < fields_.size() &&
        fields_[focus_].pointer(event)) {
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && body_.contains(event.x, event.y)) {
        const float delta = event.wheel_y != 0.0F ? event.wheel_y * 36.0F : event.scroll_y;
        if (max_form_scroll_ > 0 &&
            (!list_.contains(event.x, event.y) || (delta < 0 && offset_ == 0) ||
             form_offset_ < max_form_scroll_)) {
            form_offset_ = std::clamp(form_offset_ + delta, 0.0F, max_form_scroll_);
            redraw_();
        } else {
            scroll(delta);
        }
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (body_.contains(event.x, event.y) && fields_[i].bounds().contains(event.x, event.y)) {
            focus_ = i;
            fields_[i].pointer(event);
            redraw_();
            return;
        }
    }
    if (cancel_.contains(event.x, event.y)) {
        close();
    } else if (apply_button_.contains(event.x, event.y)) {
        submit();
    } else if (body_.contains(event.x, event.y) && list_.contains(event.x, event.y)) {
        focus_ = kPreview;
        redraw_();
    }
}

void BatchRenameDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                             const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    if (busy_) {
        const float width = std::min(480.0F, bounds.width - 32.0F);
        const ui::Rect card{bounds.x + (bounds.width - width) / 2.0F,
                            bounds.y + (bounds.height - 100.0F) / 2.0F, width, 100.0F};
        renderer.draw_box(card, {.fill = palette.surface_container_high,
                                 .radius = ui::Corners::all(14.0F),
                                 .border_width = 1.0F,
                                 .border = palette.outline_variant});
        renderer.draw_text(busy_message_, card.x + 20.0F, card.y + 24.0F, 16.0F, palette.on_surface,
                           width - 40.0F);
        renderer.draw_text(i18n::tr("Please wait…"), card.x + 20.0F, card.y + 56.0F, 13.0F,
                           palette.on_surface_variant, width - 40.0F);
        return;
    }
    const float width = std::max(240.0F, std::min(780.0F, bounds.width - 24.0F));
    const float height = std::min(590.0F, bounds.height - 24.0F);
    const ui::Rect card{bounds.x + (bounds.width - width) / 2.0F,
                        bounds.y + (bounds.height - height) / 2.0F, width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14.0F),
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    const float left = card.x + 18.0F;
    const float room = width - 36.0F;
    float y = card.y + 16.0F;
    renderer.draw_text(i18n::tr("Batch Rename"), left, y, 17.0F, palette.on_surface, room);
    y += 28.0F;
    const float buttons_y = card.bottom() - 50.0F;
    body_ = {left, y, room, std::max(30.0F, buttons_y - 42.0F - y)};
    const bool wide = room >= 600.0F;
    const float form_height = wide ? 178.0F : 232.0F;
    const float list_height = std::max(96.0F, body_.height - form_height);
    max_form_scroll_ = std::max(0.0F, form_height + list_height - body_.height);
    form_offset_ = std::clamp(form_offset_, 0.0F, max_form_scroll_);
    y -= form_offset_;
    renderer.push_clip(body_);
    renderer.draw_text(
        i18n::tr("Names keep their extension with {ext}. Numbering follows the view order."), left,
        y, 12.0F, palette.on_surface_variant, room);
    y += 24.0F;
    const auto field = [&](std::size_t i, float x, float w, float top) {
        renderer.draw_text(i18n::tr(labels[i]), x, top, 12.0F, palette.on_surface_variant, w);
        fields_[i].draw(
            renderer, {x, top + 18.0F, w, 28.0F}, 13.0F,
            {.background = palette.surface_container_highest,
             .border = focus_ == i ? palette.primary : palette.outline_variant,
             .text = palette.on_surface,
             .selection = palette.secondary_container,
             .caret = focus_ == i ? palette.on_surface : palette.surface_container_highest});
    };
    const float half = (room - 12.0F) / 2.0F;
    field(0, left, half, y);
    field(1, left + half + 12.0F, half, y);
    y += 54.0F;
    field(2, left, wide ? half : room, y);
    if (!wide) {
        y += 54.0F;
    }
    const float third = ((wide ? half : room) - 24.0F) / 3.0F;
    const float numbers_x = wide ? left + half + 12.0F : left;
    for (std::size_t i = 3; i < 6; ++i) {
        field(i, numbers_x + static_cast<float>(i - 3) * (third + 12.0F), third, y);
    }
    y += 54.0F;
    renderer.draw_text(
        i18n::tr("{name} = name after replacement     {n} = number     {ext} = extension"), left, y,
        12.0F, palette.on_surface_variant, room);
    y += 24.0F;
    renderer.draw_text(i18n::tr("Old name"), left + 6.0F, y, 13.0F, palette.on_surface_variant,
                       half - 12.0F);
    renderer.draw_text(i18n::tr("New name"), left + half + 12.0F, y, 13.0F,
                       palette.on_surface_variant, half - 12.0F);
    y += 22.0F;
    list_ = {left, y, room, list_height};
    max_scroll_ = std::max(0.0F, static_cast<float>(preview_.changes.size()) * kRow - list_.height);
    offset_ = std::clamp(offset_, 0.0F, max_scroll_);
    renderer.draw_box(list_,
                      {.fill = palette.surface_container_low,
                       .radius = ui::Corners::all(4.0F),
                       .border_width = 1.0F,
                       .border = focus_ == kPreview ? palette.primary : palette.outline_variant});
    renderer.push_clip(list_);
    for (std::size_t i = 0; i < preview_.changes.size(); ++i) {
        const float row_y = list_.y + static_cast<float>(i) * kRow - offset_;
        if (row_y + kRow < list_.y || row_y >= list_.bottom()) {
            continue;
        }
        const auto& change = preview_.changes[i];
        const auto color =
            change.from == change.to ? palette.on_surface_variant : palette.on_surface;
        renderer.draw_text(change.from, left + 6.0F, row_y + 4.0F, 13.0F, color, half - 12.0F);
        renderer.draw_text(change.to, left + half + 12.0F, row_y + 4.0F, 13.0F,
                           change.sidecar ? palette.on_surface_variant : color, half - 18.0F);
    }
    renderer.pop_clip();
    if (max_scroll_ > 0) {
        const float thumb =
            std::max(16.0F, list_.height * list_.height / (list_.height + max_scroll_));
        renderer.fill_rect({list_.right() - 4.0F,
                            list_.y + (list_.height - thumb) * offset_ / max_scroll_, 3.0F, thumb},
                           palette.outline);
    }
    renderer.pop_clip();  // scrollable fields and preview; buttons stay visible
    std::string status = preview_.error;
    if (status.empty()) {
        const auto count =
            std::ranges::count_if(preview_.changes, [](const auto& c) { return c.from != c.to; });
        status = count == 0 ? std::string{i18n::tr("No names would change")}
                            : i18n::trn("{} name will change, including sidecars",
                                        "{} names will change, including sidecars",
                                        static_cast<std::uint64_t>(count));
    }
    renderer.draw_text(status, left, buttons_y - 32.0F, 12.0F,
                       preview_.error.empty() ? palette.on_surface_variant : palette.error, room);
    apply_button_ = {card.right() - 130.0F, buttons_y, 112.0F, 32.0F};
    cancel_ = {apply_button_.x - 120.0F, buttons_y, 112.0F, 32.0F};
    for (const bool apply : {false, true}) {
        const auto& button = apply ? apply_button_ : cancel_;
        const bool enabled = !apply || can_apply();
        renderer.draw_box(
            button, {.fill = apply && enabled ? palette.primary : palette.surface_container_highest,
                     .radius = ui::Corners::all(6.0F),
                     .border_width = 1.0F,
                     .border = focus_ == (apply ? kApply : kCancel) ? palette.primary
                                                                    : palette.outline_variant});
        draw_button_text(renderer, i18n::tr(apply ? "Rename" : "Cancel"), button,
                         apply && enabled ? palette.on_primary : palette.on_surface_variant);
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
