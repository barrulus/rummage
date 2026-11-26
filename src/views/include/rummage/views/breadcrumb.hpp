#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/i18n.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// One step of a path in the breadcrumb: the home or the root as an icon, then a folder's name.
struct Crumb {
    std::filesystem::path path;
    std::string label;      // empty for the first crumb, which is an icon
    std::string_view icon;  // set for the first crumb only

    bool operator==(const Crumb&) const = default;
};

// folder's steps: from the home (an icon) when folder is in it, else from the root.
[[nodiscard]] std::vector<Crumb> crumbs_of(const std::filesystem::path& folder,
                                           const std::filesystem::path& home);

// The bar above the view: the folder's path as clickable steps, cut from the left with "…" when
// it does not fit (the first step and the last ones stay). A click on a step asks to go there,
// with the cursor on the step after it, the folder we came from.
//
// Loop thread only.
class Breadcrumb {
public:
    // go(path, child): the step at path was clicked; child is the name of the step after it
    // (empty for the last).
    using Go = std::function<void(const std::filesystem::path& path, const std::string& child)>;

    Breadcrumb(std::function<void()> redraw, Go go, std::filesystem::path home,
               core::WorkerPool* pool = nullptr);

    ~Breadcrumb() { completion_job_.cancel(); }

    void set_status_handler(std::function<void(std::string, bool)> status) {
        status_ = std::move(status);
    }

    [[nodiscard]] bool completing() const { return completing_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder);

    // The pointer at (x, y) is over the bar as last drawn.
    [[nodiscard]] bool claims(float x, float y) const;

    // A crumb navigates, or this part of the bar opens the path editor.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    [[nodiscard]] bool text_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event);

    void set_submit_handler(std::function<std::string(std::string)> submit) {
        submit_ = std::move(submit);
    }

    void set_copy_handler(std::function<void(std::string)> copy) { copy_ = std::move(copy); }

    [[nodiscard]] bool editing() const { return editing_; }

    [[nodiscard]] const std::string& text() const { return field_.text(); }

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] std::uint64_t edit_generation() const { return edit_generation_; }

    void cancel_edit() {
        if (!editing_)
            return;
        invalidate_completion();
        editing_ = false;
        ++edit_generation_;
        error_.clear();
        redraw_();
    }

    void begin_submit() { submitting_ = true; }

    void finish_submit(std::string error) {
        submitting_ = false;
        error_ = std::move(error);
        if (error_.empty())
            cancel_edit();
        redraw_();
    }

    void edit(const std::filesystem::path& path, bool select_all = true);
    bool key(const ui::KeyEvent& event);
    void paste(std::string_view text);
    std::string copy_selection(bool cut);

    // The folder of the step at a window point, as last drawn (nullopt: none, or the "…").
    [[nodiscard]] std::optional<std::filesystem::path> path_at(float x, float y) const;

    // A drag over the window would drop into the step at this path (nullopt: none), drawn
    // marked.
    void set_drop_target(std::optional<std::filesystem::path> path);

    // The steps shown in the last draw: the first one's icon name, then the folders' names, "…"
    // where steps were left out (tests).
    [[nodiscard]] std::vector<std::string> shown() const;

    [[nodiscard]] ui::TextField* input_field() {
        field_.set_accessible_name(std::string{i18n::tr("Location")});
        return editing_ ? &field_ : nullptr;
    }

private:
    void complete(bool reverse);
    void invalidate_completion();
    void choose_completion();
    core::WorkerPool* pool_;
    core::CancelToken completion_job_;
    bool completing_{false};
    bool submitting_{false};
    std::vector<std::string> completions_;
    std::string completed_text_;
    std::ptrdiff_t completion_index_{0};
    std::function<void(std::string, bool)> status_;

    struct Shown {
        std::optional<std::size_t> crumb;  // nullopt: the "…"
        ui::Rect rect{};
    };

    [[nodiscard]] std::optional<std::size_t> crumb_at(float x, float y) const;

    std::function<void()> redraw_;
    Go go_;
    std::function<std::string(std::string)> submit_;
    std::function<void(std::string)> copy_;
    ui::TextField field_;
    bool editing_{false};
    std::uint64_t edit_generation_{0};
    std::filesystem::path path_;
    std::string error_;
    std::filesystem::path home_;
    std::vector<Crumb> crumbs_;  // as last drawn
    std::vector<Shown> shown_;
    ui::Rect bounds_{};
    std::optional<std::size_t> hovered_;  // a crumb
    std::optional<std::filesystem::path> drop_target_;
};

}  // namespace rummage::views
