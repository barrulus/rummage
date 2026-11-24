#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rummage/fs/directory_model.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/subfolders.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/list_size.hpp"

namespace rummage::views {

// The place that holds path: the one with the longest path that path is in (Pictures before
// Home, Home before the file system); nullopt when none does.
[[nodiscard]] std::optional<std::size_t> place_holding(std::span<const fs::Place> places,
                                                       const std::filesystem::path& path);

// The places at the left of the window, in sections (places, bookmarks, devices), the one that
// holds the current folder marked. A click opens a place; so does Enter when the keyboard is in
// the sidebar (the browser moves it there and back), where Up/Down, Home/End move a cursor and
// Escape leaves; a right click asks for a place's menu. Its right edge drags to resize it; a double
// click there restores the width.
//
// Loop thread only.
class Sidebar {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kWidth = 200.0F;  // at start, and after a double click on its edge
    static constexpr float kMinWidth = 140.0F;
    static constexpr float kMaxWidth = 400.0F;
    // The view at its right keeps at least this: the sidebar narrows, then is left out.
    static constexpr float kMinContent = 480.0F;
    static constexpr float kEdgeGrab = 4.0F;  // either side of the edge, where the pointer grabs
    static constexpr float kRowHeight = 30.0F;

    // open: a place was chosen (click or Enter); a disk not mounted has no path yet.
    Sidebar(std::function<void()> redraw, std::function<void(ui::CursorShape)> set_cursor,
            std::function<void(const fs::Place&)> open);

    void enable_tree(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                     std::shared_ptr<const fs::MimeDatabase> mime);
    bool loading() const;
    bool expandable(std::size_t row) const;

    void set_show_hidden(bool hidden);
    void toggle_folder(const std::filesystem::path& path, std::string_view root = {});

    static bool child(const fs::Place& place) { return place.icon_key == "sidebar_child"; }

    const std::vector<fs::Place>& sources() const { return sources_; }

    static std::string place_key(const fs::Place& place);

    static bool separator(const fs::Place& place) { return place.icon_key == "sidebar_separator"; }

    void set_organization(std::vector<std::string> order, std::vector<std::string> hidden,
                          std::vector<std::string> collapsed);

    const auto& order() const { return order_; }

    const auto& hidden() const { return hidden_; }

    const auto& collapsed() const { return collapsed_; }

    void hide_place(const fs::Place& place);
    void restore_places();
    void move_place(const fs::Place& place, int direction);
    void add_separator(const fs::Place& before);
    void remove_separator(const fs::Place& place);
    void toggle_section(fs::Place::Section section);

    void set_close_handler(std::function<void()> close) { close_ = std::move(close); }

    void set_background_menu(std::function<void(float, float)> menu) {
        background_menu_ = std::move(menu);
    }

    ui::Rect close_bounds() const { return close_bounds_; }

    // The eject icon of an external disk was clicked.
    void set_eject_handler(std::function<void(const fs::Place&)> eject) {
        eject_ = std::move(eject);
    }

    // Where the eject icon of the place at a window point is, as last drawn; nullopt when it
    // has none (tests).
    [[nodiscard]] std::optional<ui::Rect> eject_icon(const std::filesystem::path& path) const;

    void set_places(std::vector<fs::Place> places);

    // A right click on a place: menu(place, x, y).
    using MenuHandler = std::function<void(const fs::Place& place, float x, float y)>;

    void set_menu_handler(MenuHandler menu) { menu_ = std::move(menu); }

    [[nodiscard]] const std::vector<fs::Place>& places() const noexcept { return places_; }

    [[nodiscard]] std::optional<ui::Rect> place_icon_rect(const std::filesystem::path& path) const;

    // current: the folder shown, whose place is marked.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const std::filesystem::path& current, Clock::time_point now);

    // Left out of this frame (no room, or hidden): it takes no pointer events.
    void not_drawn() noexcept {
        bounds_ = {};
        close_bounds_ = {};
        drag_.reset();
        moving_separator_.reset();
        hovered_.reset();
        hovered_eject_.reset();
        pointer_inside_ = false;
    }

    // Its width in a window this wide: narrowed to leave the view kMinContent, 0 when even
    // kMinWidth does not fit.
    [[nodiscard]] float width_in(float window_width) const;

    [[nodiscard]] float width() const noexcept { return width_; }

    // Clamped to [kMinWidth, kMaxWidth].
    void set_width(float width);

    // Independent of the main view; scales rows, labels and icons together.
    void set_list_size(std::size_t index);

    [[nodiscard]] std::size_t list_size() const noexcept { return list_size_; }

    // The pointer at (x, y) is the sidebar's: over it or its edge, or dragging the edge.
    [[nodiscard]] bool claims(float x, float y) const;

    // An eject icon is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event, Clock::time_point now);

    // A key while the keyboard is in the sidebar: false when it is not one of its keys. Enter
    // and Escape hand the keyboard back (focused() turns false).
    bool key(const ui::KeyEvent& event);

    // The keyboard is in the sidebar: its cursor shows, starting on the current place.
    void set_focused(bool focused);

    [[nodiscard]] bool focused() const noexcept { return focused_; }

    [[nodiscard]] const fs::Place* hovered_place() const {
        return pointer_inside_ && !bounds_.empty() && hovered_ && *hovered_ < places_.size()
                   ? &places_[*hovered_]
                   : nullptr;
    }

    void draw_drive_tooltip(ui::Renderer& renderer, const theme::Palette& palette,
                            const ui::Rect& window, std::string_view text) const;

    [[nodiscard]] std::optional<std::size_t> cursor() const noexcept { return cursor_; }

    [[nodiscard]] const fs::Place* cursor_place() const {
        return cursor_ && *cursor_ < places_.size() ? &places_[*cursor_] : nullptr;
    }

    [[nodiscard]] std::pair<float, float> cursor_anchor() const;

    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    // The place at a window point, as last drawn.
    [[nodiscard]] const fs::Place* place_under(float x, float y) const;

    // A drag over the window would drop into the place at this path (nullopt: none), drawn
    // marked.
    void set_drop_target(std::optional<std::filesystem::path> path);

    // Where a drop adds bookmarks: the "BOOKMARKS" title, or the free space below the places.
    [[nodiscard]] bool in_bookmark_zone(float x, float y) const;

    // A drag over that zone: a line shows where the bookmarks would go (after the last one).
    void set_bookmark_drop(bool shown);

    // Bookmarks renamed from the sidebar (an empty label gives the folder's name back).
    using RenameHandler =
        std::function<void(const std::filesystem::path& folder, const std::string& label)>;

    void set_rename_handler(RenameHandler rename) { rename_ = std::move(rename); }

    // A text field over the label of folder's bookmark, its label selected. Enter or a press
    // elsewhere confirms, Escape cancels. Nothing when folder is not a bookmark.
    void start_rename(const std::filesystem::path& folder);

    [[nodiscard]] bool renaming() const noexcept { return renaming_.has_value(); }

    // The label's field, while renaming.
    [[nodiscard]] const ui::TextField& field() const noexcept { return field_; }

    // While renaming every key is the field's (true).
    bool rename_key(const ui::KeyEvent& event);

    // While renaming: a press in the field is the field's (true); elsewhere it confirms and
    // returns false, so the press goes on to what is under it.
    bool rename_pointer(const ui::PointerEvent& event);

    // Text pasted into the field (its first line, control characters left out).
    void paste(std::string_view text);

    // While renaming: the selected text, taken out of the label.
    std::string cut();

    void confirm_rename();
    void cancel_rename();

    // F2 renames a bookmark; Alt+Up/Down moves a Places entry or separator locally.
    // True for these keys even off a bookmark: they are the sidebar's, never the view's.
    bool edit_key(const ui::KeyEvent& event);

    // The cursor on the place at path, when it is listed (a bookmark pointed elsewhere).
    void set_cursor_to(const std::filesystem::path& path);

private:
    struct Row {
        float top{0.0F};                   // content coordinates
        std::optional<std::size_t> place;  // nullopt: the title of section
        fs::Place::Section section{fs::Place::Section::places};
    };

    [[nodiscard]] float scale() const noexcept {
        return kListSizes[list_size_].row / kListSizes[kUsualListSize].row;
    }

    void rebuild();
    using NodeId = std::pair<std::string, std::filesystem::path>;
    void append_tree(fs::Place place, std::size_t depth, const std::string& root,
                     bool link = false);
    void toggle_row(std::size_t index);
    void release_models();
    void expand_folder(const std::filesystem::path& path);
    std::function<std::unique_ptr<fs::DirectoryModel>()> make_model_;
    std::map<std::filesystem::path, std::unique_ptr<fs::DirectoryModel>> models_;
    std::unique_ptr<fs::Subfolders> subfolders_;
    std::set<NodeId> expanded_;
    std::vector<NodeId> node_ids_;
    std::vector<std::size_t> depths_;
    std::vector<bool> links_;
    std::vector<fs::Place> sources_;
    std::vector<std::string> order_, hidden_, collapsed_;
    bool tree_hidden_{false}, rebuilding_{false};
    ui::Rect close_bounds_{};
    std::function<void()> close_;
    std::function<void(float, float)> background_menu_;
    void layout();
    [[nodiscard]] std::optional<std::size_t> place_at(float x, float y) const;
    // The place whose eject icon is at a point (a little wider than the icon).
    [[nodiscard]] std::optional<std::size_t> eject_at(float x, float y) const;
    [[nodiscard]] bool on_edge(float x, float y) const;
    void update_hover();
    void choose(std::size_t place);

    std::function<void()> redraw_;
    std::function<void(ui::CursorShape)> set_cursor_;
    std::function<void(const fs::Place&)> open_;
    std::function<void(const fs::Place&)> eject_;
    // A place's eject icon, by place, as last drawn (empty for the others).
    std::vector<ui::Rect> eject_rects_;
    std::optional<std::size_t> hovered_eject_;
    MenuHandler menu_;
    std::vector<fs::Place> places_;
    std::vector<Row> rows_;
    float content_height_{0.0F};
    std::optional<std::size_t> current_;  // the place holding the folder shown, as last drawn
    std::optional<std::size_t> cursor_;   // while focused
    std::optional<std::size_t> hovered_;
    std::optional<std::filesystem::path> drop_target_;
    bool bookmark_drop_{false};
    bool focused_{false};
    float width_{kWidth};
    std::size_t list_size_{kUsualListSize};
    float zoom_wheel_{0.0F};
    ui::Rect bounds_{};  // as last drawn; empty when not drawn
    ui::Scroller scroller_;

    struct Drag {
        float start_x;
        float start_width;
    };

    std::optional<Drag> drag_;
    std::optional<fs::Place> moving_separator_;
    RenameHandler rename_;
    ui::TextField field_;                            // the label being edited
    std::optional<std::filesystem::path> renaming_;  // whose bookmark
    std::string renaming_from_;                      // its label when the edit began
    bool over_edge_{false};
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
};

}  // namespace rummage::views
