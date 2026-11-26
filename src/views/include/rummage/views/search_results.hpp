#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "rummage/core/i18n.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/file_finder.hpp"

namespace rummage::views {

// A tab-local results view. Selection is keyed by root-relative path, never by row number.
class SearchResults {
public:
    struct Snapshot {
        std::filesystem::path root;
        std::string query;
        bool hidden{false};
        std::set<std::string> selected;
        std::string cursor, anchor;
        std::size_t first{0};
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);

    enum class Action {
        copy,
        cut,
        trash,
        rename,
        preview,
        reveal,
        open,
        save,
        saved,
        refresh,
        stop,
        details,
        close,
        undo,
        redo
    };
    SearchResults(core::WorkerPool& pool, std::function<void()> redraw,
                  std::function<void(Action)> action);
    void open(const std::filesystem::path& root, std::string query, bool hidden);
    void close();
    void refresh();
    void set_query(std::string query);
    void select(std::size_t row, ui::Modifiers modifiers = {});
    void select_all();
    void key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    void draw(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette,
              bool focused = true);

    void focus_query() {
        focus_ = -1;
        redraw_();
    }

    void paste(std::string_view text);
    std::string copy_query(bool cut);

    void message(std::string message) {
        message_ = std::move(message);
        redraw_();
    }

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool editing() const { return focus_ == -1; }

    [[nodiscard]] bool loading() const {
        return finder_ && (finder_->scanning() || finder_->matching());
    }

    [[nodiscard]] FileFinder* finder() const { return finder_.get(); }

    [[nodiscard]] const std::vector<FoundFile>& rows() const;
    [[nodiscard]] std::vector<std::filesystem::path> chosen() const;
    [[nodiscard]] ui::Rect control_bounds(Action action) const;

    [[nodiscard]] ui::Rect list_bounds() const { return list_; }

    [[nodiscard]] std::size_t first_row() const { return first_; }

    [[nodiscard]] ui::TextField* input_field() {
        query_.set_accessible_name(std::string{i18n::tr("Search Results")});
        return open_ && editing() ? &query_ : nullptr;
    }

private:
    struct Control {
        Action action;
        ui::Rect bounds;
        bool enabled;
    };

    void updated();
    void move(int delta, ui::Modifiers modifiers);
    void run(Action action);
    [[nodiscard]] bool enabled(Action action) const;
    [[nodiscard]] std::size_t cursor_index() const;
    void reveal_cursor();
    core::WorkerPool& pool_;
    std::function<void()> redraw_;
    std::function<void(Action)> action_;
    std::unique_ptr<FileFinder> finder_;
    ui::TextField query_;
    std::set<std::string> selected_;
    std::string cursor_, anchor_, message_;
    std::vector<Control> controls_;
    ui::Rect list_{};
    std::size_t first_{0}, visible_{8};
    int focus_{-1};  // -1 query, -2 list, otherwise toolbar index
    float row_height_{52}, wheel_rest_{0};
    bool open_{false};
    std::optional<Snapshot> restoring_;
};
}  // namespace rummage::views
