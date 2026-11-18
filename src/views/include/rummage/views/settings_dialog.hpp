#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/config_edit.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/theme.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/context_menu.hpp"
#include "rummage/views/error_dialog.hpp"
#include "rummage/views/menu_editor.hpp"
#include "rummage/views/settings_editors.hpp"

namespace rummage::views {

class SettingsDialog {
public:
    SettingsDialog(core::WorkerPool& pool, std::function<void()> redraw);
    ~SettingsDialog();
    void open(std::filesystem::path path, std::function<void()> open_file,
              std::function<void(const core::ConfigResult&)> applied);
    // Commits valid edits and waits for writes. Unresolved drafts/failures require explicit
    // discard.
    void close();

    bool is_open() const { return open_; }

    bool busy() const { return busy_; }

    std::uint64_t generation() const { return generation_; }

    const std::string& error() const { return error_; }

    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    ui::CursorShape cursor_at(float x, float y) const;
    void paste(std::string_view text);
    std::string copy_selection(bool cut);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons);
    // Commit pending text fields / retry a failed save. Successful saves keep Settings open.
    void save();
    bool edit(std::string_view table, std::string_view key, std::string value);
    void add_action();
    void add_keybinding();

    // Accept a complete logical edit; disk persistence remains serialized by this host.
    std::string apply_edits(const std::vector<core::ConfigEdit>& edits,
                            std::string_view append = {});

    const core::ConfigResult& accepted_config() const { return accepted_config_; }

    bool editable() const { return valid_ && read_only_reason_.empty(); }

    const std::string& read_only_reason() const { return read_only_reason_; }

    bool reset_setting(std::string_view table, std::string_view key);
    bool reset_category(int section);
    void search(std::string query);

    struct SearchResult {
        std::size_t field;
        int section;
        std::string label, description;
    };

    std::vector<SearchResult> search_results() const;
    void jump_to_result(std::size_t field);

    // Reports applied state, independently of unsaved edits in this dialog.
    void set_theme_status_handler(std::function<theme::Theme::Status()> status) {
        theme_status_ = std::move(status);
        if (open_)
            redraw_();
    }

    [[nodiscard]] std::string palette_status() const;

private:
    struct Number {
        float minimum, maximum, step;
    };

    struct Field {
        int section{0};
        std::string table, key, label, original;
        bool raw{false};
        // Reset removes this override so the parser remains the source of default values.
        bool resettable{false};
        ui::TextField input;
        std::vector<std::string> choices;
        ui::Rect bounds{};
        std::string group, description, error;
        std::optional<Number> number;
        float top{0}, height{0};

        bool toggle() const {
            return choices.size() == 2 && choices[0] == "true" && choices[1] == "false";
        }
    };

    struct Button {
        std::string id, label;
        ui::Rect bounds;
    };

    bool refresh_editability();
    void refresh_values(const std::vector<core::ConfigEdit>& reset);
    void populate(const core::ConfigResult& config);
    void activate(std::string_view id, bool keyboard = false);
    static std::string choice_label(const Field& field, std::string_view value);
    void change_section(int section);
    void set_focus(std::string focus);
    void commit_focus();
    void commit(std::size_t index);
    void flush();
    void update_error();
    bool has_drafts() const;
    void finish_close();
    void maybe_close();
    void reveal_focus();
    void layout_rows();
    void step_number(std::size_t index, int direction);
    core::WorkerPool& pool_;
    std::function<void()> redraw_, open_file_;
    std::function<void(const core::ConfigResult&)> applied_;
    std::function<theme::Theme::Status()> theme_status_;
    core::CancelToken job_;
    std::filesystem::path path_;
    // original_ is the last disk snapshot; staged_ includes validated edits queued behind a write.
    std::string original_, staged_, error_, write_error_, read_only_reason_;
    core::ConfigResult accepted_config_;
    ui::TextField search_;
    ui::Rect search_bounds_{};
    bool open_{false}, busy_{false}, existed_{false}, valid_{false}, saved_{false};
    bool close_requested_{false}, discard_prompt_{false};
    std::uint64_t generation_{0};
    std::size_t action_count_{0}, new_actions_{0}, new_bindings_{0};
    std::vector<Field> fields_;
    std::vector<Button> buttons_;
    int section_{0};
    std::string focus_{"section0"};
    ui::Scroller scroller_;
    ui::Scrollbar scrollbar_;
    ui::Rect list_{}, last_card_{};
    ContextMenu choices_;
    ErrorDialog details_;
    SettingsEditors editors_;
    MenuEditor menus_;
    ui::Rect editor_bounds_{};
    bool editor_focus_pending_{false}, editor_reverse_focus_{false};
};
}  // namespace rummage::views
