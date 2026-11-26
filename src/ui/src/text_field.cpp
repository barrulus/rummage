#include "rummage/ui/text_field.hpp"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/ui/text.hpp"

namespace rummage::ui {
namespace {

constexpr float kCaretWidth = 1.5F;

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

// Characters that end a word for Ctrl+arrows: in a file name, the dots, dashes and underscores
// as well as spaces.
bool separator(char c) {
    return c == ' ' || c == '.' || c == '_' || c == '-' || c == '\t' || c == ',' || c == '(' ||
           c == ')' || c == '[' || c == ']';
}

// Text a key may type: no control characters.
bool typable(std::string_view text) {
    if (text.empty())
        return false;
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x20U || first == 0x7fU)
            return false;
        if (first < 0x80U)
            continue;
        const unsigned length = first >= 0xc2U && first <= 0xdfU   ? 2U
                                : first >= 0xe0U && first <= 0xefU ? 3U
                                : first >= 0xf0U && first <= 0xf4U ? 4U
                                                                   : 0U;
        if (!length || text.size() - at < length - 1)
            return false;
        std::uint32_t scalar = first & ((1U << (7U - length)) - 1U);
        for (unsigned n = 1; n < length; ++n) {
            const auto byte = static_cast<unsigned char>(text[at++]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            scalar = (scalar << 6U) | (byte & 0x3fU);
        }
        if ((length == 3 && scalar < 0x800U) || (length == 4 && scalar < 0x10000U) ||
            (scalar >= 0xd800U && scalar <= 0xdfffU) || scalar > 0x10ffffU)
            return false;
    }
    return true;
}

}  // namespace

TextField::TextField(std::string text) { set_text(std::move(text)); }

void TextField::set_text(std::string text) {
    dragging_ = false;
    preedit_.clear();
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
    scroll_ = 0.0F;
    undo_.clear();
    redo_.clear();
    last_edit_ = Edit::other;
}

void TextField::remember(Edit kind) {
    if (kind == Edit::other || kind != last_edit_) {
        undo_.push_back({.text = text_, .caret = caret_, .anchor = anchor_});
        if (undo_.size() > kHistory) {
            undo_.erase(undo_.begin());
        }
    }
    redo_.clear();
    last_edit_ = kind;
}

void TextField::restore(State state) {
    text_ = std::move(state.text);
    caret_ = std::min(state.caret, text_.size());
    anchor_ = std::min(state.anchor, text_.size());
    last_edit_ = Edit::other;
}

bool TextField::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back({.text = text_, .caret = caret_, .anchor = anchor_});
    State state = std::move(undo_.back());
    undo_.pop_back();
    restore(std::move(state));
    return true;
}

bool TextField::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back({.text = text_, .caret = caret_, .anchor = anchor_});
    State state = std::move(redo_.back());
    redo_.pop_back();
    restore(std::move(state));
    return true;
}

std::size_t TextField::previous(std::size_t at) const noexcept {
    if (at == 0) {
        return 0;
    }
    --at;
    while (at > 0 && continuation(text_[at])) {
        --at;
    }
    return at;
}

std::size_t TextField::next(std::size_t at) const noexcept {
    if (at >= text_.size()) {
        return text_.size();
    }
    ++at;
    while (at < text_.size() && continuation(text_[at])) {
        ++at;
    }
    return at;
}

std::size_t TextField::previous_word(std::size_t at) const noexcept {
    if (masked_) {
        return 0;
    }
    while (at > 0 && separator(text_[previous(at)])) {
        at = previous(at);
    }
    while (at > 0 && !separator(text_[previous(at)])) {
        at = previous(at);
    }
    return at;
}

std::size_t TextField::next_word(std::size_t at) const noexcept {
    if (masked_) {
        return text_.size();
    }
    while (at < text_.size() && separator(text_[at])) {
        at = next(at);
    }
    while (at < text_.size() && !separator(text_[at])) {
        at = next(at);
    }
    return at;
}

void TextField::select(std::size_t begin, std::size_t end) {
    dragging_ = false;
    const auto boundary = [this](std::size_t at) {
        at = std::min(at, text_.size());
        while (at > 0 && at < text_.size() && continuation(text_[at])) {
            --at;
        }
        return at;
    };
    anchor_ = boundary(begin);
    caret_ = boundary(end);
    last_edit_ = Edit::other;
}

std::pair<std::size_t, std::size_t> TextField::selection() const noexcept {
    return {std::min(caret_, anchor_), std::max(caret_, anchor_)};
}

std::string TextField::selected_text() const {
    if (masked_)
        return {};
    const auto [first, last] = selection();
    return text_.substr(first, last - first);
}

void TextField::move_to(std::size_t at, bool extend) {
    last_edit_ = Edit::other;  // typing after a move is a new edit
    caret_ = at;
    if (!extend) {
        anchor_ = at;
    }
}

void TextField::erase(std::size_t first, std::size_t last) {
    text_.erase(first, last - first);
    caret_ = first;
    anchor_ = first;
}

void TextField::insert(std::string_view text) {
    remember(Edit::other);
    const auto [first, last] = selection();
    text_.replace(first, last - first, text);
    caret_ = first + text.size();
    anchor_ = caret_;
}

std::string TextField::cut() {
    if (masked_)
        return {};
    const auto [first, last] = selection();
    if (first == last) {
        return {};
    }
    std::string taken = text_.substr(first, last - first);
    remember(Edit::other);
    erase(first, last);
    return taken;
}

bool TextField::key(const KeyEvent& event) {
    if (event.input_edit)
        return input_edit(*event.input_edit);
    dragging_ = false;
    const bool shift = event.modifiers.shift;
    const bool ctrl = event.modifiers.ctrl;
    const auto [first, last] = selection();
    const bool selected = first != last;
    switch (event.keysym) {
        case XKB_KEY_Left:
            if (selected && !shift) {
                move_to(first, false);  // to the selection's start
            } else {
                move_to(ctrl ? previous_word(caret_) : previous(caret_), shift);
            }
            return true;
        case XKB_KEY_Right:
            if (selected && !shift) {
                move_to(last, false);
            } else {
                move_to(ctrl ? next_word(caret_) : next(caret_), shift);
            }
            return true;
        case XKB_KEY_Home:
        case XKB_KEY_Up:
            move_to(0, shift);
            return true;
        case XKB_KEY_End:
        case XKB_KEY_Down:
            move_to(text_.size(), shift);
            return true;
        case XKB_KEY_BackSpace:
        case XKB_KEY_Delete: {
            std::pair<std::size_t, std::size_t> gone{first, last};
            if (!selected) {
                gone = event.keysym == XKB_KEY_BackSpace
                           ? std::pair{ctrl ? previous_word(caret_) : previous(caret_), caret_}
                           : std::pair{caret_, ctrl ? next_word(caret_) : next(caret_)};
            }
            if (gone.first != gone.second) {
                // A selection removed, or a word, is an edit of its own; characters go on.
                remember(selected || ctrl ? Edit::other : Edit::removing);
                erase(gone.first, gone.second);
            }
            return true;
        }
        default:
            break;
    }
    if (ctrl && !event.modifiers.alt && (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A)) {
        select_all();
        return true;
    }
    if (ctrl && !event.modifiers.alt && (event.keysym == XKB_KEY_z || event.keysym == XKB_KEY_Z)) {
        if (shift) {
            redo();
        } else {
            undo();
        }
        return true;
    }
    if (!ctrl && !event.modifiers.alt && typable(event.text)) {
        // Characters typed one after another are one edit, the selection they replaced included.
        remember(Edit::typing);
        const auto [from, to] = selection();
        text_.replace(from, to - from, event.text);
        caret_ = from + event.text.size();
        anchor_ = caret_;
        return true;
    }
    return false;
}

void TextField::focus_input(TextField* field) {
    if (input_focus_ != field && field)
        field->preedit_.clear();
    input_focus_ = field;
}

TextInputState TextField::input_state() const {
    TextInputState state{.identity = this,
                         .text = {},
                         .cursor = 0,
                         .anchor = 0,
                         .password = masked_,
                         .cursor_rect = caret_bounds_};
    if (masked_)
        return state;
    // Protocol messages have a 4000-byte surrounding-text limit. Include only complete
    // UTF-8 characters; omit an overlong selection instead of publishing invalid indices.
    const auto [first, last] = selection();
    if (last - first > 3000)
        return state;
    std::size_t start = first > 500 ? first - 500 : 0;
    while (start < text_.size() && continuation(text_[start]))
        ++start;
    std::size_t end = std::min(text_.size(), start + 4000);
    while (end > start && end < text_.size() && continuation(text_[end]))
        --end;
    if (last > end)
        return state;
    state.text = text_.substr(start, end - start);
    state.cursor = static_cast<std::int32_t>(caret_ - start);
    state.anchor = static_cast<std::int32_t>(anchor_ - start);
    return state;
}

bool TextField::input_edit(const TextInputEdit& edit) {
    const auto boundary = [](std::string_view text, std::size_t at) {
        return at <= text.size() && (at == text.size() || !continuation(text[at]));
    };
    const auto [first, last] = selection();
    // Never split UTF-8 or accept offsets outside the state supplied to the IME.
    if (edit.before > first || edit.after > text_.size() - last ||
        !boundary(text_, first - edit.before) || !boundary(text_, last + edit.after))
        return false;
    if ((edit.commit && !edit.commit->empty() && !typable(*edit.commit)) ||
        (!edit.preedit.empty() && !typable(edit.preedit)) || edit.preedit.size() > 4000 ||
        (edit.commit && edit.commit->size() > 65536))
        return false;
    preedit_.clear();
    if (edit.before || edit.after || edit.commit || (!edit.preedit.empty() && first != last)) {
        remember(Edit::other);
        // Surrounding deletion excludes the selection. A commit or a new preedit
        // replaces it; a deletion-only transaction preserves its direction and text.
        text_.erase(last, edit.after);
        text_.erase(first - edit.before, edit.before);
        caret_ -= edit.before;
        anchor_ -= edit.before;
        if (edit.commit || !edit.preedit.empty()) {
            const auto from = first - edit.before;
            const auto committed = edit.commit.value_or("");
            text_.replace(from, last - first, committed);
            caret_ = anchor_ = from + committed.size();
        }
    }
    preedit_ = edit.preedit;
    const auto clamp = [&](std::int32_t at) {
        if (at < 0)
            return std::int32_t{-1};
        auto value = std::min(static_cast<std::size_t>(at), preedit_.size());
        while (value > 0 && !boundary(preedit_, value))
            --value;
        return static_cast<std::int32_t>(value);
    };
    preedit_begin_ = clamp(edit.begin);
    preedit_end_ = clamp(edit.end);
    return true;
}

std::string TextField::display_text(std::size_t bytes) const {
    bytes = std::min(bytes, text_.size());
    if (!masked_)
        return text_.substr(0, bytes);
    std::string bullets;
    for (std::size_t at = 0; at < bytes; at = next(at))
        bullets += "•";
    return bullets;
}

float TextField::width_of(FontSystem& fonts, std::size_t bytes) const {
    if (bytes == 0) {
        return 0.0F;
    }
    return fonts.layout(display_text(bytes), size_, scale_).width / scale_;
}

void TextField::click(float x, bool extend) {
    if (fonts_ == nullptr) {
        return;
    }
    const float wanted = x - (bounds_.x + kPadding) + scroll_;
    std::size_t best = 0;
    float best_distance = wanted < 0.0F ? 0.0F : wanted;
    for (std::size_t at = next(0); at <= text_.size(); at = next(at)) {
        const float distance = std::abs(width_of(*fonts_, at) - wanted);
        if (distance < best_distance) {
            best = at;
            best_distance = distance;
        }
        if (at == text_.size()) {
            break;
        }
    }
    move_to(best, extend);
}

bool TextField::pointer(const PointerEvent& event) {
    using Kind = PointerEvent::Kind;
    if (event.kind == Kind::press) {
        dragging_ = false;
        if (event.button != PointerButton::left || !bounds_.contains(event.x, event.y)) {
            return false;
        }
        dragging_all_ = event.click_count >= 2;
        if (dragging_all_) {
            select_all();
        } else {
            click(event.x, event.modifiers.shift);
        }
        dragging_ = true;
        return true;
    }
    if (!dragging_) {
        return false;
    }
    if (event.kind == Kind::leave) {
        dragging_ = false;
        return true;
    }
    if (event.kind == Kind::motion ||
        (event.kind == Kind::release && event.button == PointerButton::left)) {
        if (!dragging_all_) {
            click(event.x, true);
        }
        if (event.kind == Kind::release) {
            dragging_ = false;
        }
        return true;
    }
    return false;
}

void TextField::draw(Renderer& renderer, const Rect& rect, float size, const FieldColors& colors,
                     bool masked) {
    masked_ = masked || always_masked_;
    if (AccessibleFrame::current()) {
        core::AccessibleNode node;
        node.id = "field:" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        node.name = accessible_name_;
        node.role = masked_ ? core::AccessibleRole::password : core::AccessibleRole::entry;
        node.password = masked_;
        node.editable = accessible_editable_;
        node.focused = input_focus_ == this;
        node.enabled = accessible_editable_;
        node.text = display_text(text_.size());
        node.caret = core::text_character_offset(text_, caret_);
        node.anchor = core::text_character_offset(text_, anchor_);
        renderer.accessible(std::move(node), rect);
    }

    bounds_ = rect;
    size_ = size;
    scale_ = renderer.scale();
    fonts_ = &renderer.fonts();
    renderer.draw_box(
        rect,
        {.fill = colors.background, .radius = {}, .border_width = 1.5F, .border = colors.border});

    // Sideways scroll: the caret stays inside, and a text that fits is not scrolled.
    const Rect inner = rect.inset(kPadding);
    const float room = std::max(1.0F, inner.width - kCaretWidth);
    const bool composing = input_focus_ == this && !preedit_.empty();
    const auto [selection_first, selection_last] = selection();
    auto composing_text = masked_ ? std::string{} : preedit_;
    if (masked_)
        for (char byte : preedit_)
            if (!continuation(byte))
                composing_text += "•";
    const float preedit_width =
        composing ? fonts_->layout(composing_text, size_, scale_).width / scale_ : 0;
    const float start_x = width_of(*fonts_, composing ? selection_first : caret_);
    const float total = width_of(*fonts_, text_.size()) + preedit_width;
    const float caret_x =
        start_x + (composing && preedit_begin_ >= 0
                       ? fonts_->layout(masked_ ? composing_text
                                                : preedit_.substr(
                                                      0, static_cast<std::size_t>(preedit_begin_)),
                                        size_, scale_)
                                 .width /
                             scale_
                       : 0);
    if (total <= room) {
        scroll_ = 0.0F;
    } else {
        scroll_ = std::clamp(scroll_, caret_x - room, caret_x);
        scroll_ = std::clamp(scroll_, 0.0F, total - room);
    }

    renderer.push_clip({inner.x, rect.y, inner.width, rect.height});  // sideways: descenders stay
    const float x = inner.x - scroll_;
    const float line = size + 6.0F;
    const float line_y = rect.y + ((rect.height - line) / 2.0F);
    const auto [first, last] = selection();
    if (first != last && !composing) {
        const float from = width_of(*fonts_, first);
        const float to = width_of(*fonts_, last);
        renderer.fill_rect({x + from, line_y, to - from, line}, colors.selection);
    }
    auto shown = display_text(text_.size());
    if (composing) {
        const auto prefix = display_text(selection_first);
        const auto suffix = display_text(selection_last);
        shown = prefix + composing_text + shown.substr(suffix.size());
        renderer.fill_rect({x + start_x, line_y + line - 2, preedit_width, 1.5F}, colors.text);
        if (!masked_ && preedit_begin_ >= 0 && preedit_end_ > preedit_begin_) {
            const float left =
                fonts_
                    ->layout(preedit_.substr(0, static_cast<std::size_t>(preedit_begin_)), size_,
                             scale_)
                    .width /
                scale_;
            const float right =
                fonts_
                    ->layout(preedit_.substr(0, static_cast<std::size_t>(preedit_end_)), size_,
                             scale_)
                    .width /
                scale_;
            renderer.fill_rect({x + start_x + left, line_y, right - left, line}, colors.selection);
        }
    }
    renderer.draw_text(shown, x, rect.y + ((rect.height - size) / 2.0F) - 2.0F, size, colors.text);
    caret_bounds_ = {std::clamp(x + caret_x, inner.x, inner.right()), line_y, kCaretWidth, line};
    if (!composing || preedit_begin_ >= 0)
        renderer.fill_rect(caret_bounds_, colors.caret);
    renderer.pop_clip();
}

}  // namespace rummage::ui
