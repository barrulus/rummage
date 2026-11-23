#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rummage/fs/insights.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/code_stats.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// Area preserving binary treemap. Zero weights get empty rectangles; never enlarged for labels.
[[nodiscard]] std::vector<ui::Rect> insight_treemap(const std::vector<double>& weights,
                                                    ui::Rect bounds);

class InsightsDialog {
public:
    enum class View { types, space, matches, largest, age, age_matches, languages, code_matches };

    struct Row {
        std::string label{}, extension{};
        std::size_t node{0};
        std::uint64_t files{0}, bytes{0};
        bool directory{false}, partial{false};
        platform::CodeCounts code{};
        std::filesystem::path path{};
        std::size_t depth{0};
    };

    using Reveal = std::function<void(const std::filesystem::path&, bool)>;
    InsightsDialog(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
                   fs::InsightOptions options = {});
    ~InsightsDialog();
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
    void back();
    void up();
    void navigate(const std::filesystem::path& path);

    [[nodiscard]] bool can_back() const { return !history_.empty(); }

    [[nodiscard]] bool can_up() const;

    [[nodiscard]] std::filesystem::path current_path() const { return scope_path(); }

    void key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const fs::MimeDatabase& mime);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool loading() const { return loading_ || source_.running(); }

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

    void remember();
    void restore(Location location);
    void rebuild();
    void page(int direction);
    void reveal(std::size_t node);
    [[nodiscard]] std::filesystem::path scope_path() const;
    [[nodiscard]] std::string status() const;
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
