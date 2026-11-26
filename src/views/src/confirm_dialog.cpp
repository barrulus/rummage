#include "rummage/views/confirm_dialog.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/views/dialog_chrome.hpp"

namespace rummage::views {
namespace {

constexpr float kCardWidth = 480.0F;
constexpr float kPadding = 24.0F;
constexpr float kTextSize = 13.0F;
constexpr float kButtonHeight = 34.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kActionWidth = 116.0F;

bool contains(const ui::Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

}  // namespace

ConfirmDialog::ConfirmDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ConfirmDialog::open(std::string title, std::string detail, std::string action, Answer answer,
                         std::string warning) {
    ++generation_;
    password_mode_ = false;
    field_masked_ = true;
    name_accept_ = nullptr;
    password_field_.set_text({});
    warning_ = std::move(warning);
    title_ = std::move(title);
    detail_ = std::move(detail);
    action_ = std::move(action);
    answer_ = std::move(answer);
    hovered_.reset();
    focused_ = static_cast<int>(kAction);
    redraw_();
}

void ConfirmDialog::open_password(std::string name, bool retry,
                                  std::function<void(std::optional<std::string>)> answer,
                                  std::string action) {
    open(
        i18n::trf("Password for “{}”", name),
        std::string{retry ? i18n::tr("Password rejected or archive damaged. Try again.")
                          : i18n::tr("Enter the archive password.")},
        action.empty() ? std::string{i18n::tr("Extract")} : std::move(action),
        [this, answer = std::move(answer)](bool confirmed) {
            std::optional<std::string> password;
            if (confirmed)
                password = password_field_.text();
            password_field_.set_text({});
            answer(std::move(password));
        },
        "");
    password_mode_ = true;
    focused_ = -1;
}

void ConfirmDialog::open_name(std::string title, std::string detail, std::string initial,
                              std::function<std::string(const std::string&)> accept,
                              std::string label) {
    open(
        std::move(title), std::move(detail),
        label.empty() ? std::string{i18n::tr("Compress")} : std::move(label), [](bool) {}, "");
    password_mode_ = true;
    focused_ = -1;
    field_masked_ = false;
    password_field_.set_masked(false);
    password_field_.set_text(std::move(initial));
    password_field_.select_all();
    name_accept_ = std::move(accept);
}

std::string ConfirmDialog::copy_selection(bool cut) {
    if (!is_open() || !name_accept_)
        return {};
    auto& field = password_field_;
    auto text = cut ? field.cut() : field.selected_text();
    if (cut && !text.empty()) {
        redraw_();
    }
    return text;
}

void ConfirmDialog::paste(std::string_view text) {
    if (is_open() && password_mode_ &&
        text.find_first_of(std::string_view{"\r\n\0", 3}) == std::string_view::npos) {
        password_field_.insert(text);
        redraw_();
    }
}

void ConfirmDialog::set_detail(std::string detail) {
    detail_ = std::move(detail);
    redraw_();
}

void ConfirmDialog::close() {
    if (answer_) {
        answer_ = nullptr;
        password_field_.set_text({});
        redraw_();
    }
}

void ConfirmDialog::choose(bool confirmed) {
    if (confirmed && name_accept_) {
        const std::string error = name_accept_(password_field_.text());
        if (!error.empty()) {
            set_detail(error);
            return;
        }
    }
    name_accept_ = nullptr;
    Answer answer = std::exchange(answer_, nullptr);
    redraw_();
    if (answer) {
        answer(confirmed);
    }
}

bool ConfirmDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        const int first = password_mode_ ? -1 : 0;
        const int count = password_mode_ ? 3 : 2;
        focused_ =
            first +
            (focused_ - first +
             (event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ? count - 1 : 1)) %
                count;
        redraw_();
        return true;
    }
    if (password_mode_ && focused_ == -1 && event.keysym != XKB_KEY_Return &&
        event.keysym != XKB_KEY_KP_Enter && event.keysym != XKB_KEY_Escape) {
        if (event.text.find_first_of(std::string_view{"\r\n\0", 3}) == std::string::npos)
            password_field_.key(event);
        redraw_();
        return true;
    }
    if (event.modifiers.ctrl || event.modifiers.alt) {
        return true;
    }
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(focused_ != static_cast<int>(kCancel));
            break;
        case XKB_KEY_Escape:
            choose(false);
            break;
        default:
            break;
    }
    return true;
}

void ConfirmDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    if (password_mode_ && password_field_.pointer(event)) {
        focused_ = -1;
        redraw_();
        return;
    }
    std::optional<std::size_t> over;
    if (event.kind != Kind::leave) {
        for (std::size_t i = 0; i < buttons_.size(); ++i) {
            if (contains(buttons_.at(i), event.x, event.y)) {
                over = i;
            }
        }
    }
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind == Kind::press && event.button == ui::PointerButton::left && over) {
        choose(*over == kAction);
    }
}

void ConfirmDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                         const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    const auto frame = DialogFrame::fit(bounds, kCardWidth, 250);
    const auto card = frame.card;
    frame.draw(renderer, bounds, palette, title_);
    renderer.push_clip(card);
    const float left = card.x + kPadding;
    const float room = std::max(1.0F, card.width - (2.0F * kPadding));
    float y = frame.body.y + 16;
    renderer.draw_text(detail_, left, y, kTextSize, palette.on_surface, room);
    y += 22.0F;
    if (password_mode_) {
        password_field_.draw(renderer, {left, y - 2.0F, room, 30.0F}, kTextSize,
                             {.background = palette.surface_container_highest,
                              .border = palette.primary,
                              .text = palette.on_surface,
                              .selection = palette.primary_container,
                              .caret = palette.primary},
                             field_masked_);
    } else {
        renderer.draw_text(warning_, left, y, kTextSize, palette.on_surface_variant, room);
    }
    y = frame.footer.y + 10;

    const float button_width = std::min(kActionWidth, std::max(1.0F, (room - kButtonGap) / 2));
    buttons_.at(kAction) = {card.right() - kPadding - button_width, y, button_width, kButtonHeight};
    buttons_.at(kCancel) = {buttons_.at(kAction).x - kButtonGap - button_width, y, button_width,
                            kButtonHeight};
    for (std::size_t i = 0; i < kButtons; ++i) {
        const ui::Rect& button = buttons_.at(i);
        const bool action = i == kAction;
        const std::string label = action ? action_ : std::string{i18n::tr("Cancel")};
        draw_dialog_button(
            renderer, button, palette, label, focused_ == static_cast<int>(i) || hovered_ == i,
            action ? (password_mode_ ? DialogButton::primary : DialogButton::destructive)
                   : DialogButton::normal);
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
