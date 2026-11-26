#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "rummage/core/file_picker.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/photos/source_watcher.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/confirm_dialog.hpp"
#include "rummage/views/context_menu.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/photo_properties.hpp"

namespace rummage::views {

// Catalog controller for a normal browser tab. The browser's existing views render folder();
// only small native modal cards are drawn here. Filesystem locations and catalog identities
// remain separate, including for photographs that share a filename.
class PhotoLibrary {
public:
    using Json = nlohmann::json;
    using Id = std::int64_t;
    using FolderChooser = std::function<void(const std::filesystem::path&,
                                             std::function<void(std::filesystem::path)>)>;
    using PathChooser = std::function<void(
        const core::PickerRequest&, std::function<void(std::optional<std::filesystem::path>)>)>;
    enum class Destination { library, sources, albums, filters, review, duplicate_group, activity };

    PhotoLibrary(core::EventLoop&, core::WorkerPool&, fs::DirectoryWatcher&,
                 std::shared_ptr<const fs::MimeDatabase>, std::function<void()> redraw);
    ~PhotoLibrary();
    void open();
    void resume();
    void close();
    void refresh();
    void show(Destination);
    void show_all_photos();
    [[nodiscard]] bool at_all_photos() const;
    void add_folder(std::filesystem::path initial = {});

    // Sidebar lists are independent of the current result page. Construction and subscription
    // are inert; only explicit activation/expansion calls refresh_navigation().
    void set_navigation_handler(std::function<void(Json)> handler) {
        navigation_changed_ = std::move(handler);
    }

    void refresh_navigation();

    [[nodiscard]] const Json& navigation() const { return navigation_; }

    [[nodiscard]] std::string navigation_uri() const;
    void open_album(Id id, std::string name);
    void open_source(Id id, std::string path);
    void open_saved_search(Json record);

    [[nodiscard]] const std::shared_ptr<Folder>& folder() const { return folder_; }

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool loading() const;

    [[nodiscard]] Destination destination() const { return destination_; }

    [[nodiscard]] bool photo_rows() const;
    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::string status() const;

    [[nodiscard]] const std::string& background_status() const { return progress_; }

    [[nodiscard]] const std::string& query_text() const { return search_; }

    void set_query(std::string);
    void set_sort(const fs::SortOrder&);
    [[nodiscard]] std::vector<MenuItem> menu_items();
    [[nodiscard]] std::vector<MenuItem> context_menu_items();
    bool activate();

    void set_changed_handler(std::function<void()> changed) { changed_ = std::move(changed); }

    void set_folder_chooser(FolderChooser choose) { choose_folder_ = std::move(choose); }

    void set_path_chooser(PathChooser choose) { choose_path_ = std::move(choose); }

    void set_album_chooser(std::function<void(std::vector<MenuItem>, std::string)> choose) {
        choose_album_ = std::move(choose);
    }

    void cancel_album_chooser() { ++album_chooser_generation_; }

    void set_file_action(std::function<void(Json)> action) { file_action_ = std::move(action); }

    void set_edit_handler(std::function<void()> edit) { edit_ = std::move(edit); }

    void set_reveal_handler(std::function<void(std::filesystem::path)> reveal) {
        reveal_ = std::move(reveal);
    }

    void report(std::string);

    [[nodiscard]] std::shared_ptr<photos::Client> client() const { return client_; }

    [[nodiscard]] PhotoProperties properties(bool focused_only = false) const;
    void save_annotations(std::vector<Id>, PhotoEdits, std::function<void(std::string)>);
    [[nodiscard]] Json selected_ids() const;
    [[nodiscard]] Json selected_photos() const;
    [[nodiscard]] Json focused_photo() const;

    [[nodiscard]] const Json& rows() const { return rows_; }

    [[nodiscard]] const std::set<Id>& selection() const;
    [[nodiscard]] const std::string& state() const;
    void restore_state(std::string_view);
    void restore_cursor(std::string_view key);

    [[nodiscard]] bool has_dialog() const;
    void draw_dialogs(ui::Renderer&, const ui::Rect&, const theme::Palette&);
    bool key_dialog(const ui::KeyEvent&);
    bool pointer_dialog(const ui::PointerEvent&);
    [[nodiscard]] bool editing() const;

    [[nodiscard]] std::uint64_t edit_generation() const { return edit_generation_; }

    void paste(std::string_view);
    std::string copy_selection(bool cut);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    [[nodiscard]] ui::Rect control_bounds(std::string_view label) const;

private:
    enum class Form { none, source, filters, relink, outcome };

    struct Control {
        std::string label;
        ui::Rect bounds;
        std::function<void()> run;
        bool enabled;
    };

    void changed();
    void request(Json, std::function<void(Json)>, core::Priority = core::Priority::visible);
    void mutate(Json, std::string success);
    void query();
    void publish();
    void sync_selection() const;
    void work();
    void schedule_work(bool busy = false);
    void refresh_progress();
    void update_progress(const Json&);
    void update_background(const Json&);
    void control_background(Json);
    void page(int direction);
    void add_to_album();
    void create_album(Json ids);
    void name_dialog(std::string title, std::string detail, std::string action,
                     std::function<void(std::string)>);
    void open_form(Form, std::vector<std::pair<std::string, std::string>> fields = {});
    void close_form();
    void submit_form();
    void choose_annotations(bool exporting);
    void transfer_annotations(bool exporting, const std::filesystem::path&);
    void relink_source(const Json&);
    void review_group(const Json&);
    void confirm_duplicates();
    fs::Entry photo_entry(const Json&) const;
    Json focused_record() const;
    std::string record_key(const Json&, std::size_t) const;
    void form_button(ui::Renderer&, const theme::Palette&, ui::Rect, std::string,
                     std::function<void()>, bool enabled = true);
    float draw_wrapped(ui::Renderer&, std::string_view, float x, float y, float width, ui::Color,
                       float size = 13);

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_, changed_, edit_;
    FolderChooser choose_folder_;
    std::function<void(std::vector<MenuItem>, std::string)> choose_album_;
    PathChooser choose_path_;
    std::function<void(std::filesystem::path)> reveal_;
    std::function<void(Json)> file_action_;
    std::function<void(Json)> navigation_changed_;
    Json navigation_{
        {"albums", Json::array()}, {"filters", Json::array()}, {"sources", Json::array()}};
    std::uint64_t navigation_generation_{};
    bool navigation_loading_{false}, navigation_again_{false}, navigation_ready_{false};
    Id active_saved_search_{};
    std::shared_ptr<photos::Client> client_;
    std::shared_ptr<Folder> folder_;
    std::unique_ptr<photos::SourceWatcher> source_watcher_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::map<std::uint64_t, core::CancelToken> pending_;
    core::CancelToken query_job_;
    core::EventLoop::SourceId work_timer_{};
    std::uint64_t task_ids_{}, generation_{}, progress_generation_{}, background_generation_{};
    std::uint64_t album_chooser_generation_{};
    bool open_{false}, querying_{false}, working_{false}, publishing_{false};
    Destination destination_{Destination::library};
    Json rows_ = Json::array(), records_ = Json::array(), filter_ = Json::object();
    std::string collection_name_;
    Json duplicate_group_ = Json::object();
    std::map<std::string, Json> record_by_key_;
    mutable std::set<Id> selected_;
    mutable std::map<Id, Json> selected_snapshots_;
    mutable Id cursor_{};
    mutable std::uint64_t selection_revision_{};
    mutable std::string cached_state_;
    mutable bool state_dirty_{true};
    std::set<Id> keepers_;
    std::string search_, message_, progress_, background_pace_{"normal"};
    bool background_paused_{false}, background_ready_{false};
    int work_delay_ms_{250};
    std::size_t offset_{}, total_{};
    static constexpr std::size_t page_size_ = 200;
    fs::SortOrder order_;

    ConfirmDialog confirm_;
    Form form_{Form::none};
    std::array<ui::TextField, 8> fields_;
    std::array<std::string, 8> labels_;
    int field_count_{}, focus_{};
    std::vector<Control> controls_;
    std::uint64_t edit_generation_{}, form_generation_{};
    ui::Rect form_body_{};
    ui::Scroller form_scroll_;
    ui::Scrollbar form_scrollbar_;
    float form_height_{};
    std::string form_title_, form_message_, form_detail_;
    std::filesystem::path source_path_;
    Id source_id_{};
    bool recursive_{true}, relink_reviewed_{false};
};
}  // namespace rummage::views
