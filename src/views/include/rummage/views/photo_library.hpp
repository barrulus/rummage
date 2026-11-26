#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/photos/source_watcher.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// Collection rows and captured actions use catalog identities, never filenames or positions.
// All catalog requests run on workers; only delivered replies mutate this window's state.
class PhotoLibrary {
public:
    using Json = nlohmann::json;
    using Id = std::int64_t;
    enum class Action { close, preview, reveal };
    PhotoLibrary(core::EventLoop& loop, core::WorkerPool& pool, Thumbnails* thumbnails,
                 std::function<void()> redraw,
                 std::function<void(Action, std::filesystem::path)> action,
                 fs::DirectoryWatcher* watcher = nullptr);
    ~PhotoLibrary();
    void open();
    void resume();
    [[nodiscard]] const std::string& state() const;
    void restore_state(std::string_view state);
    void close();
    void refresh();

    void report(std::string message) {
        message_ = std::move(message);
        redraw_();
    }

    [[nodiscard]] std::shared_ptr<photos::Client> client() const { return client_; }

    void set_file_action(std::function<void(Json)> action) { file_action_ = std::move(action); }

    void add_folder(std::filesystem::path initial = {});
    void key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void draw(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette);
    void paste(std::string_view text);
    std::string copy_selection(bool cut);

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] const std::string& background_status() const { return progress_; }

    [[nodiscard]] bool loading() const { return querying_ || working_ || !pending_.empty(); }

    [[nodiscard]] const Json& rows() const { return rows_; }

    [[nodiscard]] const std::set<Id>& selection() const { return selected_; }

    [[nodiscard]] ui::Rect row_bounds(std::size_t index) const {
        for (const auto& row : row_hits_)
            if (row.index == index)
                return row.bounds;
        return {};
    }

    [[nodiscard]] ui::Rect control_bounds(std::string_view label) const {
        for (const auto& control : controls_)
            if (control.label == label)
                return control.bounds;
        return {};
    }

    [[nodiscard]] bool editing() const { return focus_ >= 0 && focus_ < field_count_; }

    [[nodiscard]] std::uint64_t edit_generation() const { return edit_generation_; }

    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

private:
    enum class Destination { library, review, activity, sources, albums, filters };
    enum class Form {
        none,
        source,
        inspect,
        filters,
        album,
        save_filter,
        export_data,
        restore_data,
        duplicates,
        relink,
        forget,
        operation
    };

    struct Control {
        std::string label;
        ui::Rect bounds;
        std::function<void()> run;
        bool enabled{true};
    };

    struct RowHit {
        ui::Rect bounds;
        std::size_t index;
    };

    void request(Json input, std::function<void(Json)> done,
                 core::Priority priority = core::Priority::visible);
    void query();
    void search_changed();
    void refresh_progress();
    void update_progress(const Json& jobs);
    void update_background(const Json& response);
    void control_background(Json changes);
    const std::vector<std::string>& wrapped_path(ui::Renderer&, const std::string&, float);
    void work();
    void schedule_work(bool busy = false);
    void mutate(Json input, std::string success);
    void switch_to(Destination destination);
    void inspect();
    void open_form(Form form, std::vector<std::pair<std::string, std::string>> fields);
    void close_form();
    void submit_form();
    void edit_field(std::size_t index);
    void select(std::size_t index, ui::Modifiers modifiers);
    void activate(Action action);
    void move(int delta, ui::Modifiers modifiers);
    Json selected_ids() const;
    void button(ui::Renderer&, const theme::Palette&, ui::Rect, std::string, std::function<void()>,
                bool enabled = true);
    void field(ui::Renderer&, const theme::Palette&, std::size_t, ui::Rect);
    void draw_form(ui::Renderer&, ui::Rect, const theme::Palette&);
    void draw_photos(ui::Renderer&, ui::Rect, const theme::Palette&);
    void draw_records(ui::Renderer&, ui::Rect, const theme::Palette&);
    void draw_thumbnail(ui::Renderer&, const Json&, ui::Rect, const theme::Palette&);
    std::string common_value(std::string_view key, bool& mixed) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    Thumbnails* thumbnails_;
    std::function<void()> redraw_;
    std::function<void(Action, std::filesystem::path)> action_;
    std::function<void(Json)> file_action_;
    Json duplicate_group_;
    Id source_id_{};
    std::string relink_path_;
    bool relink_reviewed_{false};
    std::set<Id> keepers_;
    std::shared_ptr<photos::Client> client_;
    fs::DirectoryWatcher* watcher_;
    std::unique_ptr<photos::SourceWatcher> source_watcher_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::map<std::uint64_t, core::CancelToken> pending_;
    std::uint64_t task_ids_{};
    core::CancelToken query_job_;
    core::EventLoop::SourceId work_timer_{};
    std::uint64_t generation_{}, edit_generation_{}, records_generation_{}, form_generation_{};
    bool open_{false}, querying_{false}, working_{false}, grid_{true}, recursive_{true};
    Destination destination_{Destination::library};
    Form form_{Form::none};
    Json rows_ = Json::array(), records_ = Json::array(), filter_ = Json::object();
    Json captured_ids_ = Json::array();
    std::set<Id> selected_;
    Id cursor_{}, anchor_{};
    std::size_t offset_{}, total_{}, first_{}, records_first_{}, columns_{1}, visible_{1};
    std::optional<std::size_t> scroll_anchor_;
    mutable bool state_dirty_{true};
    mutable std::string cached_state_;
    static constexpr std::size_t page_size_ = 200;
    int focus_{0}, field_count_{1};
    std::array<ui::TextField, 8> fields_;
    std::array<std::string, 8> labels_;
    ui::TextField search_;
    std::vector<Control> controls_;
    std::vector<RowHit> row_hits_;
    ui::Rect list_{}, form_content_{};
    float form_scroll_{}, form_content_height_{};
    std::map<std::string, std::pair<float, std::vector<std::string>>> wrapped_paths_;
    std::uint64_t progress_generation_{}, background_generation_{};
    bool background_paused_{false}, background_ready_{false};
    int work_delay_ms_{250};
    std::string background_pace_{"normal"};
    std::string progress_;
    std::string message_, form_message_;
};
}  // namespace rummage::views
