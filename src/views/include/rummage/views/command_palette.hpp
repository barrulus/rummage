#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/color.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

// One line of the command palette: a command, a place to go to, a tab.
struct PaletteItem {
    std::string id;       // stable ("command:new_tab", "go:/home/me/Downloads"): tests, recent list
    std::string label;    // what is shown and matched ("New Tab")
    std::string keys;     // the shortcut shown at the right ("Ctrl+T"); may be empty
    bool remember{true};  // goes into the recently used (not a tab: its number changes)
    std::function<void()> action;  // runs once the palette has closed
    // A file found (ROADMAP 8b): the label is a path, matched by name first and shortened in the
    // middle; Space runs quick_look, Shift+Enter open (both once the palette has closed).
    bool path{false};
    std::function<void()> quick_look;
    std::function<void()> open;
};

// The command palette (Ctrl+K, ROADMAP 8a): a card at the top of the window with a field and the
// items that match what is typed, best first (fzy), the matched letters in the accent and each
// item's shortcut at the right. With nothing typed, the recently used come first, then the rest in
// their order. Items that cannot be used now are not given to it at all.
//
// While open it takes every key and every click: typing filters; Up, Down, Page Up, Page Down
// move the highlight; Enter runs the highlighted item; Escape (or Ctrl+K again, the caller's
// business) closes. The pointer highlights the row it is over, a click runs it, the wheel scrolls,
// a click outside closes the palette and does nothing else.
//
// Spaces separate search terms. After arrow-key navigation, Space opens Quick Look;
// Shift+Space always inserts a space. Editing returns the keyboard to the field.
// Loop thread only.
class CommandPalette {
public:
    explicit CommandPalette(std::function<void()> redraw);

    // At most this many rows show; the rest scroll.
    static constexpr std::size_t kVisibleRows = 10;

    // recent: ids, most recent first. placeholder: the empty field's dim text.
    void open(std::vector<PaletteItem> items, const std::vector<std::string>& recent,
              std::string placeholder);
    // Results from elsewhere (finding files): every edit of the query goes to query_changed, and
    // set_results() fills the list.
    void open_external(std::string placeholder,
                       std::function<void(const std::string& query)> query_changed);

    // The list, best first, and a dim line under it ("Searching… 1 234 items"). The highlight
    // stays on the item it was moved to while that item is still there, else on the first.
    void set_results(std::vector<PaletteItem> items, std::string status);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // Open with results from elsewhere.
    [[nodiscard]] bool external() const noexcept { return open_ && query_changed_ != nullptr; }

    [[nodiscard]] const std::string& status() const noexcept { return status_; }

    // Called once the palette has closed, whatever closed it.
    void set_close_handler(std::function<void()> handler) { closed_ = std::move(handler); }

    // Called with an item's id just before its action runs (the recent list).
    void set_chosen_handler(std::function<void(const std::string& id)> handler) {
        chosen_ = std::move(handler);
    }

    // Replaces what is typed (tests, a paste).
    void set_query(std::string query);
    void paste(std::string_view text);

    [[nodiscard]] const std::string& query() const noexcept { return field_.text(); }

    [[nodiscard]] const ui::TextField& field() const noexcept { return field_; }

    // The selected text, taken out of the query; the list filters again.
    std::string cut();

    [[nodiscard]] const std::vector<PaletteItem>& items() const noexcept { return items_; }

    // The items shown, best first: indexes into items().
    [[nodiscard]] const std::vector<std::size_t>& shown() const noexcept { return shown_; }

    // The highlighted row: an index into shown(); none when nothing matches.
    [[nodiscard]] std::optional<std::size_t> highlighted() const noexcept { return highlighted_; }

    // The keyboard is in the list (moved there with Up, Down, Page Up, Page Down), not the field.
    [[nodiscard]] bool in_list() const noexcept { return in_list_; }

    // The first row in sight.
    [[nodiscard]] std::size_t first_row() const noexcept { return first_; }

    // Runs the highlighted item (Enter).
    void choose_highlighted();

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    // Every event while open: true.
    bool pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // Where the card was last drawn (hit tests); empty before.
    [[nodiscard]] const ui::Rect& panel() const noexcept { return panel_; }

    // The field and the list as last drawn (the pointer's shape).
    [[nodiscard]] const ui::Rect& field_bounds() const noexcept { return field_rect_; }

    [[nodiscard]] const ui::Rect& list_bounds() const noexcept { return list_; }

    [[nodiscard]] ui::TextField* input_field() {
        field_.set_accessible_name(placeholder_);
        return open_ && !in_list_ ? &field_ : nullptr;
    }

private:
    void filter();
    void move_highlight(std::ptrdiff_t rows);
    void scroll_to_highlight();
    // how: 0 the action, 1 quick_look, 2 open.
    void choose(std::size_t row, int how = 0);
    [[nodiscard]] std::optional<std::size_t> row_at(float x, float y) const;
    void draw_label(ui::Renderer& renderer, std::string_view label, bool path, float x, float y,
                    float room, ui::Color text, ui::Color matched) const;

    std::function<void()> redraw_;
    std::function<void(const std::string&)> chosen_;
    std::function<void()> closed_;
    std::function<void(const std::string&)> query_changed_;
    std::string status_;
    bool moved_{false};    // the highlight was moved by hand
    bool in_list_{false};  // the keyboard is in the list: Space is Quick Look (a file found)
    bool open_{false};
    std::vector<PaletteItem> items_;
    std::vector<std::size_t> recent_;  // indexes into items_, most recent first
    std::string placeholder_;
    ui::TextField field_;
    std::vector<std::size_t> shown_;
    std::optional<std::size_t> highlighted_;
    std::size_t first_{0};
    std::size_t rows_in_sight_{kVisibleRows};  // as last drawn (a short window shows fewer)
    float wheel_rest_{0.0F};  // part of a row scrolled by a touchpad, kept for the next event

    // As last drawn, for the pointer.
    ui::Rect panel_{};
    ui::Rect field_rect_{};
    ui::Rect list_{};
};

// A piece of a label as drawn: matched characters lit.
struct LabelRun {
    std::string text;
    bool lit{false};

    bool operator==(const LabelRun&) const = default;
};

// A label with its matched bytes (positions) fitted into room, measured by width (issue #25): the
// whole label when it fits; a path too long keeps its name and as much of its start as fits
// ("Documents/Pro…/report.pdf"); anything else too long (a name alone) keeps the part from its
// first matched character on, cut with "…" where needed, so the match stays in sight.
[[nodiscard]] std::vector<LabelRun> fit_label(std::string_view label,
                                              std::span<const std::size_t> positions, bool path,
                                              float room,
                                              const std::function<float(std::string_view)>& width);

}  // namespace rummage::views
