#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/measure.hpp"
#include "rummage/platform/code_stats.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/selectable_text.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// The info pane: a preview of what is selected in a folder and what it is. The same in every view
// (the columns draw it after their last column, the grid and the details at their right), so one
// object serves them all and keeps what it read.
//
// From top to bottom, centered: the preview (a photo's thumbnail, a text file's first lines, else
// the icon), the name and type, then information (size, pixels, dates) and a media summary.
// "More details" reveals the remaining media metadata, permissions, owner and location.
// A fixed copy button includes all properties, even when collapsed.
// Several entries selected: how many and how big;
// nothing selected: the folder itself. What the directory entry says shows at once, the rest when
// read on a worker. A folder's size (or a selection's with folders in it) is added up on a
// worker too, once the cursor rests (kMeasureDelay), showing the sum as it grows; moving on
// cancels it.
//
// Loop thread only. Draw it while drawing (thumbnail textures are made then).
class InfoPane {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kWidth = 300.0F;  // at start, and after a double click on its edge
    static constexpr float kMinWidth = 240.0F;
    static constexpr float kMaxWidth = 720.0F;
    // A view keeps at least this for its content: the pane narrows, then is left out.
    static constexpr float kMinContent = 320.0F;
    // Either side of the pane's edge, where the pointer grabs it (grid and details).
    static constexpr float kEdgeGrab = 4.0F;
    // A line of a text file's page.
    static constexpr float kTextLineHeight = 15.0F;

    // Resting on a folder starts its count; moving through folders starts nothing.
    static constexpr std::chrono::milliseconds kMeasureDelay{250};
    // How long the summary before stays drawn while the next one loads: moving through files,
    // the pane then does not flash an icon (and move what is under it) for a frame or two.
    static constexpr std::chrono::milliseconds kHold{150};

    // thumbnails may be nullptr: icons only.
    InfoPane(core::EventLoop& loop, core::WorkerPool& pool, Thumbnails* thumbnails,
             std::function<void()> redraw);
    ~InfoPane();

    InfoPane(const InfoPane&) = delete;
    InfoPane& operator=(const InfoPane&) = delete;
    InfoPane(InfoPane&&) = delete;
    InfoPane& operator=(InfoPane&&) = delete;

    // The renderer's font system must remain alive while this drawn page handles input.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder, Clock::time_point now);

    // Read-only text focus is independent of the file selection. Clicking elsewhere returns
    // Ctrl+A/C to the files; hidden or replaced content drops its selection.
    bool pointer(const ui::PointerEvent& event, Clock::time_point now);
    bool text_key(const ui::KeyEvent& event, Clock::time_point now);
    [[nodiscard]] bool text_at(float x, float y) const;
    [[nodiscard]] bool text_focused() const noexcept;
    void clear_text_focus();
    [[nodiscard]] std::string selected_text() const;

    void set_copy_handler(std::function<void(std::string)> handler) {
        copy_ = std::move(handler);
        metadata_.set_copy_handler(copy_);
    }

    [[nodiscard]] ui::Rect property_bounds(std::string_view id) const {
        return metadata_.bounds(id);
    }

    [[nodiscard]] ui::Rect copy_bounds() const { return copy_button_; }

    void set_insights_handler(std::function<void(const std::filesystem::path&)> handler) {
        insights_ = std::move(handler);
    }

    void set_code_handler(std::function<void(const std::filesystem::path&)> handler) {
        code_action_ = std::move(handler);
    }

    void set_code_report(std::shared_ptr<const platform::CodeReport> report);

    [[nodiscard]] ui::Rect code_bounds() const { return code_button_; }

    // A click at (x, y): true when the pane took a disclosure or copy action.
    bool click(float x, float y);

    // A wheel or touchpad scroll: true when it was over the pane, which scrolls when its content
    // is taller than it.
    bool scroll(const ui::PointerEvent& event, Clock::time_point now);

    // Keyboard movement through the information pane.
    enum class TextScroll : std::uint8_t { line_up, line_down, page_up, page_down };

    // Moves the pane by a few lines or by a page; false when all its content fits.
    bool scroll_text(TextScroll how, Clock::time_point now);

    [[nodiscard]] bool animating() const noexcept {
        // A hidden pane cannot advance its scrollers and must not keep requesting frames.
        return bounds_.width > 0.0F && bounds_.height > 0.0F && scroller_.animating();
    }

    [[nodiscard]] bool expanded() const noexcept { return expanded_; }

    // "More details" is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const {
        return bounds_.contains(x, y) &&
               (toggle_.contains(x, y) || insights_button_.contains(x, y) ||
                code_button_.contains(x, y) ||
                (!copy_text_.empty() && copy_button_.contains(x, y)));
    }

    // Before a frame: what it drew is gone unless it is drawn again (a view without the pane).
    void forget_layout() noexcept {
        bounds_ = {};
        toggle_ = {};
        insights_button_ = {};
        code_button_ = {};
        thumbnail_ = {};
        copy_button_ = {};
    }

    // Stop filesystem work when hidden; end_frame also covers a pane that no longer fits.
    void cancel_measurement() { measure({}, {}); }

    void end_frame() {
        if (bounds_.empty()) {
            cancel_measurement();
            clear_text_focus();
        }
    }

    // The width the user gave it (dragging its edge in the grid or the details).
    [[nodiscard]] float width() const noexcept { return width_; }

    // Clamped to [kMinWidth, kMaxWidth].
    void set_width(float width);

    // Its width in a view this wide: narrowed to leave the view kMinContent, 0 when even
    // kMinWidth does not fit.
    [[nodiscard]] float width_in(float view_width) const;

    void set_expanded(bool expanded);

    // Properties can change metadata without changing a file's modification time.
    void refresh() {
        key_.clear();
        redraw_();
    }

    // What it shows is still being read (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept {
        return job_.has_value() || size_timer_ != 0 || size_job_.has_value();
    }

    // Text thumbnail geometry and pane scrolling (tests).
    [[nodiscard]] const ui::Rect& thumbnail_bounds() const noexcept { return thumbnail_; }

    [[nodiscard]] float scroll_offset() const noexcept { return scroller_.offset(); }

    // What was read about the entry shown last; nullptr before it arrives (tests).
    [[nodiscard]] const preview::Summary* summary() const noexcept { return summary_.get(); }

    // The folder or combined selection's measured size; empty until counting starts (tests).
    [[nodiscard]] const std::optional<fs::Tally>& folder_size() const noexcept { return size_; }

    [[nodiscard]] bool folder_size_done() const noexcept { return size_done_; }

private:
    // Reads path's summary unless it is the one shown or on its way.
    // No summary (inside an archive: nothing to read from disk).
    void forget_summary();
    void drop_held();

    // The summary before is drawn while the next one loads: shown only, not to select or copy.
    [[nodiscard]] bool holding() const noexcept { return summary_ == nullptr && held_ != nullptr; }

    void request(const std::filesystem::path& path, std::int64_t modified_ns, std::string mime,
                 preview::Kind kind, std::shared_ptr<const fs::MimeDatabase> types);

    // No roots cancels any pending or running measurement.
    void measure(std::vector<std::filesystem::path> roots, std::string key);
    [[nodiscard]] std::string size_text() const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    Thumbnails* thumbnails_;
    std::function<void()> redraw_;

    std::string key_;  // what summary_ is (or will be) about
    std::optional<core::CancelToken> job_;
    std::shared_ptr<const preview::Summary> summary_;
    std::shared_ptr<const preview::Summary> held_;  // the one before, drawn while kHold lasts
    core::EventLoop::SourceId hold_timer_{0};
    std::string size_key_;  // what size_ is (or will be) about
    core::EventLoop::SourceId size_timer_{0};
    std::optional<core::CancelToken> size_job_;
    std::optional<fs::Tally> size_;  // so far
    bool size_done_{false};
    bool expanded_{false};
    float width_{kWidth};
    std::function<void(const std::filesystem::path&)> insights_;
    std::filesystem::path insights_path_;
    ui::Rect insights_button_{}, code_button_{};
    std::function<void(const std::filesystem::path&)> code_action_;
    std::shared_ptr<const platform::CodeReport> code_report_;
    ui::Rect bounds_{};  // as last drawn
    ui::Rect toggle_{};  // "More details", as last drawn; empty when not shown
    ui::Rect thumbnail_{};
    SelectableText metadata_;
    std::function<void(std::string)> copy_;
    std::string copy_text_;
    ui::Rect copy_button_{};
    bool copied_{false};
    std::string shown_key_;
    ui::Scroller scroller_;
    float content_height_{0.0F};  // of the last draw
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
