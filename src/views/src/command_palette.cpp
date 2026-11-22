#include "rummage/views/command_palette.hpp"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/views/fuzzy.hpp"

namespace rummage::views {
namespace {

constexpr float kWidth = 600.0F;
constexpr float kTop = 64.0F;   // from the window's top to the card
constexpr float kEdge = 16.0F;  // kept free between the card and the window's sides
constexpr float kPadding = 8.0F;
constexpr float kFieldHeight = 36.0F;
constexpr float kFieldText = 15.0F;
constexpr float kGap = 6.0F;  // between the field and the list
constexpr float kRowHeight = 30.0F;
constexpr float kTextSize = 13.0F;
constexpr float kTextInset = 12.0F;
constexpr float kKeysGap = 32.0F;
constexpr float kStatusHeight = 24.0F;  // the dim line under the list (finding files)
constexpr float kStatusText = 12.0F;

bool contains(const ui::Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

float text_width(ui::Renderer& renderer, std::string_view text, float size) {
    return renderer.fonts().layout(text, size, renderer.scale()).width / renderer.scale();
}

bool continuation(char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U; }

constexpr std::string_view kEllipsis = "\u2026";

// A label cut into pieces: byte ranges of the label, an empty range standing for "…".
struct Piece {
    std::size_t begin{0};
    std::size_t end{0};
};

}  // namespace

std::vector<LabelRun> fit_label(std::string_view label, std::span<const std::size_t> positions,
                                bool path, float room,
                                const std::function<float(std::string_view)>& width) {
    // Lit bytes: a matched byte lights its whole character.
    std::vector<bool> lit(label.size(), false);
    for (const std::size_t at : positions) {
        if (at >= label.size()) {
            continue;
        }
        std::size_t begin = at;
        while (begin > 0 && continuation(label[begin])) {
            --begin;
        }
        std::size_t end = at + 1;
        while (end < label.size() && continuation(label[end])) {
            ++end;
        }
        std::fill(lit.begin() + static_cast<std::ptrdiff_t>(begin),
                  lit.begin() + static_cast<std::ptrdiff_t>(end), true);
    }
    const auto text_of = [&label](const std::vector<Piece>& pieces) {
        std::string text;
        for (const Piece& piece : pieces) {
            text += piece.begin == piece.end ? kEllipsis
                                             : label.substr(piece.begin, piece.end - piece.begin);
        }
        return text;
    };
    const auto fits = [&](const std::vector<Piece>& pieces) {
        return width(text_of(pieces)) <= room;
    };
    const auto runs_of = [&](const std::vector<Piece>& pieces) {
        std::vector<LabelRun> runs;
        const auto add = [&runs](std::string_view text, bool on) {
            if (!runs.empty() && runs.back().lit == on) {
                runs.back().text += text;
            } else {
                runs.push_back({.text = std::string{text}, .lit = on});
            }
        };
        for (const Piece& piece : pieces) {
            if (piece.begin == piece.end) {
                add(kEllipsis, false);
                continue;
            }
            for (std::size_t at = piece.begin; at < piece.end;) {
                std::size_t end = at;
                while (end < piece.end && lit[end] == lit[at]) {
                    ++end;
                }
                add(label.substr(at, end - at), lit[at]);
                at = end;
            }
        }
        return runs;
    };
    // Character starts, and the end: where a cut may fall.
    std::vector<std::size_t> cuts;
    for (std::size_t at = 0; at <= label.size(); ++at) {
        if (at == label.size() || !continuation(label[at])) {
            cuts.push_back(at);
        }
    }
    // The largest i in [low, high] (indexes into cuts) for which make(i) fits; low when none
    // does (the caller checked low).
    const auto largest = [&](std::size_t low, std::size_t high, const auto& make) {
        while (low < high) {
            const std::size_t middle = low + ((high - low + 1) / 2);
            if (fits(make(middle))) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        return low;
    };

    const std::vector<Piece> whole{{0, label.size()}};
    if (fits(whole)) {
        return runs_of(whole);
    }
    const std::size_t last = cuts.size() - 1;  // the index of label.size()
    if (path) {
        // The name with its slash, after as much of the start as fits.
        const std::size_t slash = label.rfind('/');
        if (slash != std::string_view::npos && slash > 0) {
            const auto with_head = [&](std::size_t i) {
                std::vector<Piece> pieces;
                if (cuts[i] > 0) {
                    pieces.push_back({0, cuts[i]});
                }
                pieces.push_back({});
                pieces.push_back({slash, label.size()});
                return pieces;
            };
            if (fits(with_head(0))) {
                const auto slash_index =
                    static_cast<std::size_t>(std::ranges::lower_bound(cuts, slash) - cuts.begin());
                return runs_of(with_head(largest(0, slash_index, with_head)));
            }
        }
    }
    // From the first matched character on, as far as fits; then back towards the start.
    std::size_t first = 0;
    if (const auto found = std::ranges::find(lit, true); found != lit.end()) {
        first = static_cast<std::size_t>(
            std::ranges::lower_bound(cuts, static_cast<std::size_t>(found - lit.begin())) -
            cuts.begin());
    }
    const auto window = [&](std::size_t from, std::size_t to) {
        std::vector<Piece> pieces;
        if (from > 0) {
            pieces.push_back({});
        }
        if (to > from) {
            pieces.push_back({cuts[from], cuts[to]});
        }
        if (to < last) {
            pieces.push_back({});
        }
        return pieces;
    };
    const std::size_t to = largest(first, last, [&](std::size_t i) { return window(first, i); });
    if (to == first) {
        return {{.text = std::string{kEllipsis}, .lit = false}};  // not one character fits
    }
    // Room left: the characters before the first match, nearest first.
    std::size_t from = first;
    while (from > 0 && fits(window(from - 1, to))) {
        --from;
    }
    return runs_of(window(from, to));
}

CommandPalette::CommandPalette(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void CommandPalette::open(std::vector<PaletteItem> items, const std::vector<std::string>& recent,
                          std::string placeholder) {
    items_ = std::move(items);
    placeholder_ = std::move(placeholder);
    recent_.clear();
    for (const std::string& id : recent) {
        const auto it = std::ranges::find(items_, id, &PaletteItem::id);
        if (it != items_.end()) {
            const auto index = static_cast<std::size_t>(it - items_.begin());
            if (std::ranges::find(recent_, index) == recent_.end()) {
                recent_.push_back(index);
            }
        }
    }
    field_.set_text({});
    query_changed_ = nullptr;
    status_.clear();
    moved_ = false;
    in_list_ = false;
    open_ = true;
    panel_ = {};
    filter();
    redraw_();
}

void CommandPalette::open_external(std::string placeholder,
                                   std::function<void(const std::string&)> query_changed) {
    items_.clear();
    recent_.clear();
    shown_.clear();
    highlighted_.reset();
    first_ = 0;
    placeholder_ = std::move(placeholder);
    field_.set_text({});
    query_changed_ = std::move(query_changed);
    status_.clear();
    moved_ = false;
    in_list_ = false;
    open_ = true;
    panel_ = {};
    redraw_();
}

void CommandPalette::set_results(std::vector<PaletteItem> items, std::string status) {
    if (!open_) {
        return;
    }
    std::string kept;
    if (moved_ && highlighted_ && *highlighted_ < shown_.size()) {
        kept = items_[shown_[*highlighted_]].id;
    }
    items_ = std::move(items);
    status_ = std::move(status);
    shown_.resize(items_.size());
    for (std::size_t i = 0; i < shown_.size(); ++i) {
        shown_[i] = i;
    }
    highlighted_.reset();
    if (!kept.empty()) {
        const auto it = std::ranges::find(items_, kept, &PaletteItem::id);
        if (it != items_.end()) {
            highlighted_ = static_cast<std::size_t>(it - items_.begin());
        }
    }
    if (!highlighted_) {
        moved_ = false;
        first_ = 0;
        highlighted_ = shown_.empty() ? std::nullopt : std::optional<std::size_t>{0};
    }
    scroll_to_highlight();
    redraw_();
}

void CommandPalette::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    items_.clear();
    shown_.clear();
    recent_.clear();
    highlighted_.reset();
    query_changed_ = nullptr;
    status_.clear();
    panel_ = {};
    redraw_();
    if (closed_) {
        closed_();
    }
}

void CommandPalette::set_query(std::string query) {
    field_.set_text(std::move(query));
    filter();
    redraw_();
}

void CommandPalette::paste(std::string_view text) {
    // The first line, without control characters.
    std::string line;
    for (const char c : text) {
        if (c == '\n' || c == '\r') {
            break;
        }
        if (static_cast<unsigned char>(c) >= 0x20U && c != 0x7F) {
            line += c;
        }
    }
    field_.insert(line);
    in_list_ = false;
    filter();
    redraw_();
}

std::string CommandPalette::cut() {
    if (!open_) {
        return {};
    }
    std::string text = field_.cut();
    if (!text.empty()) {
        in_list_ = false;
        filter();
        redraw_();
    }
    return text;
}

void CommandPalette::filter() {
    if (query_changed_) {
        moved_ = false;
        query_changed_(field_.text());  // the results come through set_results()
        return;
    }
    shown_.clear();
    const std::string& query = field_.text();
    if (query.empty()) {
        shown_ = recent_;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (std::ranges::find(recent_, i) == recent_.end()) {
                shown_.push_back(i);
            }
        }
    } else {
        std::vector<std::pair<double, std::size_t>> scored;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (const std::optional<double> score = fuzzy_score(query, items_[i].label)) {
                scored.emplace_back(*score, i);
            }
        }
        // Best first; equal scores keep the items' order.
        std::ranges::stable_sort(scored, std::ranges::greater{},
                                 &std::pair<double, std::size_t>::first);
        for (const auto& [score, index] : scored) {
            shown_.push_back(index);
        }
    }
    first_ = 0;
    highlighted_ = shown_.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void CommandPalette::move_highlight(std::ptrdiff_t rows) {
    if (shown_.empty()) {
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(shown_.size()) - 1;
    const std::ptrdiff_t at = highlighted_ ? static_cast<std::ptrdiff_t>(*highlighted_) : 0;
    highlighted_ = static_cast<std::size_t>(std::clamp(at + rows, std::ptrdiff_t{0}, last));
    moved_ = true;
    scroll_to_highlight();
    redraw_();
}

void CommandPalette::scroll_to_highlight() {
    if (!highlighted_) {
        return;
    }
    const std::size_t rows = std::max<std::size_t>(rows_in_sight_, 1);
    if (*highlighted_ < first_) {
        first_ = *highlighted_;
    } else if (*highlighted_ >= first_ + rows) {
        first_ = *highlighted_ + 1 - rows;
    }
}

void CommandPalette::choose(std::size_t row, int how) {
    if (row >= shown_.size()) {
        return;
    }
    const PaletteItem& chosen = items_[shown_[row]];
    std::function<void()> run = chosen.action;
    if (how == 1) {
        run = chosen.quick_look;
    } else if (how == 2 && chosen.open) {
        run = chosen.open;
    }
    if (!run) {
        return;
    }
    // The palette closes first: the action may open it again (Open With…) or anything else.
    const std::string id = chosen.id;
    const bool remember = chosen.remember;
    close();
    if (remember && chosen_) {
        chosen_(id);
    }
    run();
}

void CommandPalette::choose_highlighted() {
    if (highlighted_) {
        choose(*highlighted_);
    }
}

bool CommandPalette::key(const ui::KeyEvent& event) {
    if (!open_) {
        return false;
    }
    const auto page = static_cast<std::ptrdiff_t>(std::max<std::size_t>(rows_in_sight_, 1));
    switch (event.keysym) {
        case XKB_KEY_Escape:
            close();
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (highlighted_) {
                choose(*highlighted_, event.modifiers == ui::Modifiers{.shift = true} ? 2 : 0);
            }
            return true;
        case XKB_KEY_space:
            // Quick Look for a file found; on a row without one a space is typed (a pattern may
            // have one).
            if (external() && in_list_ && event.modifiers == ui::Modifiers{} && highlighted_ &&
                items_[shown_[*highlighted_]].quick_look) {
                choose(*highlighted_, 1);
                return true;
            }
            break;  // a space in the query: terms
        case XKB_KEY_Up:
            in_list_ = true;
            move_highlight(-1);
            return true;
        case XKB_KEY_Down:
            in_list_ = true;
            move_highlight(1);
            return true;
        case XKB_KEY_Page_Up:
            in_list_ = true;
            move_highlight(-page);
            return true;
        case XKB_KEY_Page_Down:
            in_list_ = true;
            move_highlight(page);
            return true;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            return true;  // nothing to move between
        default:
            break;
    }
    const std::string before = field_.text();
    const auto caret = field_.selection();
    field_.key(event);  // what the field does not use is swallowed all the same
    if (field_.text() != before || field_.selection() != caret) {
        in_list_ = false;  // back to the field (not for Shift or Ctrl alone)
    }
    if (field_.text() != before) {
        filter();
    }
    redraw_();
    return true;
}

std::optional<std::size_t> CommandPalette::row_at(float x, float y) const {
    if (!contains(list_, x, y) || shown_.empty()) {
        return std::nullopt;
    }
    const auto row = first_ + static_cast<std::size_t>((y - list_.y) / kRowHeight);
    return row < shown_.size() && row < first_ + rows_in_sight_ ? std::optional{row} : std::nullopt;
}

bool CommandPalette::pointer(const ui::PointerEvent& event) {
    if (!open_) {
        return false;
    }
    if (field_.pointer(event)) {
        in_list_ = false;
        redraw_();
        return true;
    }
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::motion:
            if (const std::optional<std::size_t> row = row_at(event.x, event.y);
                row && row != highlighted_) {
                highlighted_ = row;
                redraw_();
            }
            break;
        case Kind::press:
            if (const std::optional<std::size_t> row = row_at(event.x, event.y)) {
                choose(*row);
            } else if (!contains(panel_, event.x, event.y)) {
                close();  // a click outside closes the palette and does nothing else
            }
            break;
        case Kind::scroll: {
            const std::size_t rows = std::max<std::size_t>(rows_in_sight_, 1);
            if (shown_.size() <= rows) {
                break;
            }
            const float steps = event.wheel_y != 0.0F ? event.wheel_y * 3.0F
                                                      : (event.scroll_y / kRowHeight) + wheel_rest_;
            const float whole = std::trunc(steps);
            wheel_rest_ = event.wheel_y != 0.0F ? 0.0F : steps - whole;
            const auto last = static_cast<std::ptrdiff_t>(shown_.size() - rows);
            first_ = static_cast<std::size_t>(
                std::clamp(static_cast<std::ptrdiff_t>(first_) + static_cast<std::ptrdiff_t>(whole),
                           std::ptrdiff_t{0}, last));
            redraw_();
            break;
        }
        default:
            break;
    }
    return true;
}

void CommandPalette::draw_label(ui::Renderer& renderer, std::string_view label, bool path, float x,
                                float y, float room, ui::Color text, ui::Color matched) const {
    FuzzyQuery query{field_.text()};
    const std::vector<std::size_t> positions =
        path ? query.path_positions(label) : query.positions(label);
    // A pattern's rows (external, not paths) have nothing of what was typed: no highlight.
    if (positions.empty() || (external() && !path)) {
        // Nothing to light: the renderer's own cut (a path keeps its start and its name).
        renderer.draw_text(label, x, y, kTextSize, text, room,
                           path ? ui::Ellipsis::middle : ui::Ellipsis::end);
        return;
    }
    const std::vector<LabelRun> runs = fit_label(
        label, positions, path, room,
        [&renderer](std::string_view piece) { return text_width(renderer, piece, kTextSize); });
    for (const LabelRun& run : runs) {
        x += renderer.draw_text(run.text, x, y, kTextSize, run.lit ? matched : text);
    }
}

void CommandPalette::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette) {
    if (!open_) {
        return;
    }
    const float width = std::min(kWidth, std::max(bounds.width - (2.0F * kEdge), 1.0F));
    const float top = bounds.y + std::min(kTop, std::max(bounds.height / 10.0F, kEdge));
    // As many rows as the window has room for, up to kVisibleRows; one at least ("No matches").
    const float status = status_.empty() ? 0.0F : kStatusHeight;
    const float fixed = (2.0F * kPadding) + kFieldHeight + kGap + status;
    const float room = bounds.bottom() - kEdge - top - fixed;
    rows_in_sight_ = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::max(room, 0.0F) / kRowHeight), 1, kVisibleRows);
    scroll_to_highlight();
    const std::size_t rows = std::max<std::size_t>(std::min(shown_.size(), rows_in_sight_), 1);
    const float height = fixed + (static_cast<float>(rows) * kRowHeight);
    panel_ = {bounds.x + ((bounds.width - width) / 2.0F), top, width, height};

    renderer.draw_shadow({panel_.x, panel_.y + 6.0F, panel_.width, panel_.height}, {}, 18.0F,
                         palette.shadow.with_alpha(0.6F));
    renderer.draw_box(panel_, {.fill = palette.surface_container_high,
                               .radius = {},
                               .border_width = 1.0F,
                               .border = palette.outline_variant});

    field_rect_ = {panel_.x + kPadding, panel_.y + kPadding, panel_.width - (2.0F * kPadding),
                   kFieldHeight};
    field_.draw(renderer, field_rect_, kFieldText,
                {.background = palette.surface_container_highest,
                 .border = palette.primary,
                 .text = palette.on_surface,
                 .selection = palette.secondary_container,
                 .caret = in_list_ ? palette.on_surface.with_alpha(0.3F) : palette.on_surface});
    if (field_.text().empty()) {
        const float text_y = field_rect_.y + ((kFieldHeight - kFieldText) / 2.0F) - 2.0F;
        renderer.draw_text(placeholder_, field_rect_.x + ui::TextField::kPadding + 6.0F, text_y,
                           kFieldText, palette.on_surface_variant,
                           field_rect_.width - (2.0F * ui::TextField::kPadding) - 6.0F);
    }

    list_ = {panel_.x + 4.0F, field_rect_.bottom() + kGap, panel_.width - 8.0F,
             static_cast<float>(rows) * kRowHeight};
    if (!status_.empty()) {
        renderer.draw_text(status_, list_.x + kTextInset,
                           list_.bottom() + ((kStatusHeight - kStatusText) / 2.0F) - 2.0F,
                           kStatusText, palette.on_surface_variant,
                           list_.width - (2.0F * kTextInset));
    }
    if (shown_.empty()) {
        // Finding files: nothing yet while the folder is being listed.
        if (!external() || !field_.text().empty()) {
            renderer.draw_text(i18n::tr("No matches"), list_.x + kTextInset,
                               list_.y + ((kRowHeight - kTextSize) / 2.0F) - 2.0F, kTextSize,
                               palette.outline);
        }
        return;
    }
    renderer.push_clip(list_);
    for (std::size_t r = 0; r < rows && first_ + r < shown_.size(); ++r) {
        const std::size_t row = first_ + r;
        const PaletteItem& item = items_[shown_[row]];
        const ui::Rect rect{list_.x, list_.y + (static_cast<float>(r) * kRowHeight), list_.width,
                            kRowHeight};
        const bool lit = highlighted_ == row;
        if (lit) {
            renderer.draw_box(rect, {.fill = palette.hover, .radius = {}});
        }
        const ui::Color text = lit ? palette.on_hover : palette.on_surface;
        const ui::Color dim = lit ? palette.on_hover : palette.on_surface_variant;
        const ui::Color matched =
            lit ? palette.on_hover
                : theme::readable_foreground(palette.primary, palette.surface_container_high);
        const float text_y = rect.y + ((kRowHeight - kTextSize) / 2.0F) - 2.0F;
        float right = rect.right() - kTextInset;
        if (!item.keys.empty()) {
            const std::string keys = i18n::keys(item.keys);
            const float keys_width = text_width(renderer, keys, kTextSize);
            renderer.draw_text(keys, right - keys_width, text_y, kTextSize, dim);
            right -= keys_width + kKeysGap;
        }
        const float label_x = rect.x + kTextInset;
        // On the accent the row is one colour: the matched letters stand out in the other rows.
        draw_label(renderer, item.label, item.path, label_x, text_y,
                   std::max(right - label_x, 1.0F), text, matched);
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
