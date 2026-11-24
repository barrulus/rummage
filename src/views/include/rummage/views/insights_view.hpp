#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rummage/fs/insights.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/code_stats.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/drop_mark.hpp"
#include "rummage/views/edit_slot.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// Area preserving binary treemap. Zero weights get empty rectangles; never enlarged for labels.
[[nodiscard]] std::vector<ui::Rect> insight_treemap(const std::vector<double>& weights,
                                                    ui::Rect bounds);

// Full-circle weighted ring. Non-positive/non-finite weights get empty sectors.
[[nodiscard]] std::vector<ui::AnnularSector> insight_ring(const std::vector<double>& weights,
                                                          float x, float y, float inner,
                                                          float outer);

struct InsightSlice {
    std::size_t node{0}, parent{0}, depth{0}, grouped{0}, first{0};
    float start{0}, sweep{0};
    std::uint64_t files{0}, bytes{0};
    bool partial{false};
};

// Three descendant levels. A child occupies its parent's angular span; files terminate a branch.
[[nodiscard]] std::vector<InsightSlice> insight_sunburst(const fs::InsightSnapshot& snapshot,
                                                         const std::vector<std::size_t>& roots,
                                                         bool bytes);

enum class InsightCategory { folders, documents, code, images, audio, video, archives, other };
[[nodiscard]] InsightCategory insight_category(std::string_view name, bool directory,
                                               const fs::MimeDatabase& mime);
[[nodiscard]] ui::Color insight_color(InsightCategory category);

class InsightsView {
public:
    enum class View {
        types,
        other_types,
        space,
        iris,
        matches,
        largest,
        age,
        age_matches,
        languages,
        code_matches
    };

    struct Row {
        std::string label{}, extension{};
        std::size_t node{0};
        std::uint64_t files{0}, bytes{0};
        bool directory{false}, partial{false}, other{false};
        platform::CodeCounts code{};
        std::filesystem::path path{};
        std::size_t depth{0};
    };

    using Reveal = std::function<void(const std::filesystem::path&, bool)>;
    InsightsView(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
                 fs::InsightOptions options = {});
    ~InsightsView();
    void open(std::filesystem::path root, bool hidden, Reveal reveal,
              std::optional<View> initial = {});

    void set_code_handler(platform::CodeAnalysis::Done done) { code_done_ = std::move(done); }

    [[nodiscard]] const platform::CodeReport* code_report() const { return code_.get(); }

    void close();
    void refresh();
    void stop();
    void set_view(View view);
    void set_size_measure(bool bytes);
    void set_recursive(bool recursive);
    void set_hidden(bool hidden);
    void activate_row(std::size_t index);
    using FolderFactory = std::function<std::shared_ptr<Folder>(const std::filesystem::path&)>;
    void set_browser_handlers(FolderFactory factory, std::function<void()> activate,
                              std::function<void(float, float)> menu, EditSlot* edit);
    void select_row(std::size_t index, ui::Modifiers modifiers = {});
    void select_all(bool invert = false);
    void set_filter(std::string query);

    enum class SortKey { name, files, bytes, lines, code, comments, blanks, share };

    struct SortColumn {
        SortKey key;
        std::string_view id, label;
    };

    [[nodiscard]] std::vector<SortColumn> sort_columns() const;
    [[nodiscard]] std::optional<SortKey> sort_key() const;
    [[nodiscard]] bool sort_descending() const;
    void set_sort(SortKey key, bool descending);

    [[nodiscard]] bool size_measure() const { return bytes_; }

    [[nodiscard]] bool recursive() const { return options_.recursive; }

    [[nodiscard]] bool hidden() const { return options_.hidden; }

    [[nodiscard]] std::shared_ptr<Folder> folder() const {
        return selected_folder_ ? selected_folder_ : scope_folder_;
    }

    [[nodiscard]] std::shared_ptr<Folder> scope_folder() const { return scope_folder_; }

    [[nodiscard]] std::optional<ViewHit> hit(float x, float y) const;
    [[nodiscard]] ui::Rect selected_bounds() const;

    void back();
    void up();
    void navigate(const std::filesystem::path& path);

    [[nodiscard]] bool can_back() const { return !history_.empty(); }

    [[nodiscard]] bool can_up() const;

    [[nodiscard]] std::filesystem::path current_path() const { return scope_path(); }

    void key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event,
                 ui::Scroller::Clock::time_point now = ui::Scroller::Clock::now());
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const fs::MimeDatabase& mime,
              ui::Scroller::Clock::time_point now = ui::Scroller::Clock::now());
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

    // Uses the app's motion/reduced-motion policy. Zero duration settles immediately.
    void set_iris_motion(float duration_seconds);

    [[nodiscard]] bool animating() const {
        return open_ && (scroller_.animating() || iris_animating_);
    }

    [[nodiscard]] ui::Rect scrollbar_thumb() const { return scrollbar_.thumb(scroller_); }

    [[nodiscard]] ui::Rect list_bounds() const { return list_; }

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool loading() const {
        return loading_ || source_.running() ||
               (scope_folder_ && scope_folder_->model().loading()) ||
               (selected_folder_ && selected_folder_->model().loading());
    }

    [[nodiscard]] View view() const { return view_; }

    [[nodiscard]] const std::vector<Row>& rows() const { return rows_; }

    [[nodiscard]] const fs::InsightSnapshot* snapshot() const { return data_.get(); }

    [[nodiscard]] std::size_t scope() const { return scope_; }

    [[nodiscard]] ui::Rect control_bounds(std::string_view id) const;

private:
    struct Control {
        std::string id;
        ui::Rect bounds;
        std::function<void()> action;
        std::optional<ui::AnnularSector> sector{};

        [[nodiscard]] bool contains(float x, float y) const {
            return sector ? sector->contains(x, y) : bounds.contains(x, y);
        }
    };

    struct Location {
        std::filesystem::path path;
        View view;
        std::string extension;
        std::size_t offset;
    };

    [[nodiscard]] bool source_view() const {
        return view_ == View::languages || view_ == View::code_matches;
    }

    void clear_selection();
    void sync_folder();
    void selection_changed();
    void finish_rows();
    void sort_rows();
    [[nodiscard]] bool file_row(std::size_t index) const;
    [[nodiscard]] std::filesystem::path row_path(std::size_t index) const;
    [[nodiscard]] bool selected(std::size_t index) const;
    bool edit_name(ui::Rect rect, std::size_t index, float size);
    void remember();
    void restore(Location location);
    void rebuild();
    void page(int direction);
    [[nodiscard]] bool chart_hover(std::size_t index) const;
    void draw_map(ui::Renderer&, ui::Rect, const theme::Palette&, ui::IconSet&,
                  const fs::MimeDatabase&);
    [[nodiscard]] ui::Color row_color(const Row&, const theme::Palette&,
                                      const fs::MimeDatabase&) const;
    void draw_legend(ui::Renderer&, ui::Rect, const theme::Palette&, const fs::MimeDatabase&) const;
    void draw_iris(ui::Renderer&, ui::Rect, const theme::Palette&, ui::IconSet&,
                   const fs::MimeDatabase&, ui::Scroller::Clock::time_point);
    void invalidate_iris();

    struct MapLayout {
        std::vector<std::size_t> shown;
        std::vector<double> weights;
        Row more;
        std::size_t grouped{0}, first{0};
        ui::Rect bounds{};
        bool dirty{true};
    };

    MapLayout map_layout_;

    struct IrisSegment {
        std::string key;
        Row row;
        ui::AnnularSector shape;
        std::size_t index{0}, parent{0}, level{0};
        float opacity{1};
        bool more{false};
    };

    std::vector<IrisSegment> iris_target_, iris_from_, iris_painted_;
    ui::Rect iris_bounds_{};
    std::shared_ptr<const fs::InsightSnapshot> iris_snapshot_;
    ui::Scroller::Clock::time_point iris_started_{};
    float iris_duration_{0.28F};
    bool iris_dirty_{true}, iris_animating_{false};

    ui::Scroller scroller_;
    ui::Scrollbar scrollbar_;
    std::optional<float> requested_scroll_{0.0F};
    void reveal(std::size_t node);
    [[nodiscard]] std::filesystem::path scope_path() const;
    [[nodiscard]] std::string status() const;
    FolderFactory folder_factory_;
    std::function<void()> activate_file_;
    std::function<void(float, float)> menu_;
    std::shared_ptr<Folder> scope_folder_, selected_folder_;
    std::optional<std::size_t> selected_row_;
    std::filesystem::path selected_path_;
    std::optional<std::pair<float, float>> pending_menu_;
    bool pending_open_{false};
    EditSlot* edit_{nullptr};
    std::string filter_;
    std::optional<SortKey> sort_;
    bool sort_descending_{false};
    std::map<std::filesystem::path, std::filesystem::path> renamed_paths_;
    core::WorkerPool& pool_;
    platform::CodeAnalysis source_;
    platform::CodeAnalysis::Done code_done_;
    std::shared_ptr<const platform::CodeReport> code_;
    std::function<void()> redraw_;
    Reveal reveal_;
    core::CancelToken job_, stop_;
    std::filesystem::path root_, entry_root_;
    std::vector<Location> history_;
    fs::InsightOptions options_;
    fs::InsightProgress progress_;
    std::shared_ptr<const fs::InsightSnapshot> data_;
    std::vector<Row> rows_;
    std::vector<Control> controls_;
    std::string extension_, focus_{"types"}, hover_;
    std::size_t scope_{0}, offset_{0}, page_size_{8}, pending_offset_{0};
    bool open_{false}, loading_{false}, bytes_{true};
    View view_{View::types};
    ui::Rect card_{}, list_{};
};

}  // namespace rummage::views
