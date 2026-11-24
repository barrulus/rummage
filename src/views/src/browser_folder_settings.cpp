#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
bool Browser::has_custom_folder_view() const {
    const auto folder = current_folder();
    return !picker_mode_ && folder && !folder->is_trash() &&
           folder_settings(pane()) != State::FolderSettings{};
}

void Browser::reset_folder_view() {
    if (!has_custom_folder_view())
        return;
    note_location();
    const auto path = pane().settings_folder;
    const State::FolderSettings defaults;
    set_sort(defaults.sort_key, defaults.sort_descending);
    set_list_size(defaults.list_size);
    set_grid_size(grid_index(defaults.grid_icon));
    pane().details.set_widths(defaults.details_widths);
    pane().details.set_columns(defaults.details_columns);
    set_mode(static_cast<Mode>(defaults.mode));
    pane().retained_folder_mode.reset();
    // Retain an explicit default record: an absent record inherits the preceding folder's view.
    folder_settings_.remember(path, defaults, true);
    pane().remembered_settings = defaults;
    show_message(std::string{i18n::tr("Default view restored for this folder")}, false);
    redraw_();
}
}  // namespace rummage::views
