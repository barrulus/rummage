#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/config_edit.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/scrollbar.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

// Pages embedded in SettingsDialog. The host owns validation against the latest text, the
// serialized write queue and errors from disk. This component never writes or runs actions.
class SettingsEditors {
public:
    enum class Page { shortcuts, actions };
    using Commit =
        std::function<std::string(const std::vector<core::ConfigEdit>&, std::string_view)>;
    explicit SettingsEditors(Commit commit, std::function<void()> redraw);
    void reset(const core::Config& config, bool editable);
    // Update accepted data without replacing a form containing an unfinished edit.
    void sync(const core::Config& config, bool editable);
    void set_page(Page page);
    void draw(ui::Renderer& renderer, const ui::Rect& body, const theme::Palette& palette);
    bool key(const ui::KeyEvent& event);
    bool pointer(const ui::PointerEvent& event);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    void paste(std::string_view text);
    [[nodiscard]] std::string copy_selection(bool cut = false);
    bool commit();
    void discard();
    [[nodiscard]] bool has_drafts() const;

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Model operations are also used by keyboard, pointer and regression tests.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> commands(
        std::string_view query = {}) const;
    bool assign_shortcut(std::string_view target, std::optional<std::string> chord);
    void record_shortcut(std::string target);
    void focus_shortcut(std::string_view target);
    void dismiss();
    void enter_focus(bool reverse = false);
    void blur();

    [[nodiscard]] bool recording() const noexcept { return !recording_.empty(); }

    [[nodiscard]] static std::optional<std::string> chord_for(const ui::KeyEvent& event);
    bool select_action(std::string_view id);
    void new_action();
    bool update_action(const core::Action& action);

private:
    enum class Operation {
        field,
        select_command,
        record,
        reset,
        disable,
        select_action,
        add_action,
        back,
        add_argument,
        remove_argument,
        selection,
        destination,
        enabled,
        add_folder,
        discard
    };

    struct Control {
        ui::Rect bounds;
        Operation operation;
        ui::TextField* field{nullptr};
        std::string value;
        std::size_t index{0};
        bool editable{true};
    };

    void activate(Control control);
    void load_action(core::Action action, bool fresh);
    [[nodiscard]] core::Action form_action() const;
    void changed();
    void reveal_focus();
    bool commit_form();
    void clear_controls();

    Commit commit_;
    std::function<void()> redraw_;
    core::Config config_;
    bool editable_{false};
    Page page_{Page::shortcuts};
    std::string target_;
    std::string recording_;
    std::string pending_chord_;
    std::string error_;
    std::optional<core::Action> action_;
    bool fresh_{false};
    ui::TextField search_, folder_, id_, label_, program_;
    std::vector<ui::TextField> arguments_;
    std::vector<Control> controls_;
    int focus_{-1};
    ui::Rect body_;
    ui::Scroller scroll_;
    ui::Scrollbar scrollbar_;
};

}  // namespace rummage::views
