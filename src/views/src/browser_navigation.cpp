#include <algorithm>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/path_completion.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::focus_files() {
    area().breadcrumb.cancel_edit();
    sidebar_.set_focused(false);
    blur_filter();
    info_pane_.clear_text_focus();
    keyboard_focus_visible_ = true;
    redraw_();
}

void Browser::edit_path() {
    focus_files();
    quick_look_.close();
    const auto folder =
        pane().mode == Mode::details ? pane().details.root_folder() : current_folder();
    if (folder)
        area().breadcrumb.edit(folder->path());
}

void Browser::open_selected_tab() {
    if (!panes_enabled_ || picker_mode_)
        return;
    const auto folder = current_folder();
    if (!folder || folder->is_trash())
        return;
    const auto names = targets(*folder);
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : folder->entries()) {
        if (std::ranges::find(names, entry.name) != names.end()) {
            if (!entry.is_directory() && !opens_as_folder(entry))
                return;
            paths.push_back(folder->path() / entry.name);
        }
    }
    if (paths.size() == 1)
        open_tab(paths.front(), true);
    else if (!paths.empty())
        open_tabs(paths);
}

std::string Browser::navigate_path(const std::string& text) {
    if (text == fs::trash_path().string()) {
        open(fs::trash_path());
        return {};
    }
    auto location = fs::parse_location(text, place_sources_.home);
    const auto origin = current_folder();
    const auto base = pane().mode == Mode::details ? pane().details.root_folder() : origin;
    if (!location && base && !text.empty() && text.find("://") == std::string::npos &&
        text.front() != '~' && text.find('\0') == std::string::npos)
        location = fs::Location{.folder = base->path() / text, .uri = {}};
    if (!location)
        return std::string{i18n::tr("Invalid path")};
    if (!location->uri.empty())
        return go_to(text, false);
    const auto generation = ++path_navigation_generation_;
    auto* owner = &area();
    auto* tab = &pane();
    area().breadcrumb.begin_submit();
    const auto edit = area().breadcrumb.edit_generation();
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [path = location->folder](const core::CancelToken&) { return fs::resolve_path(path); },
        [this, alive = alive_, generation, owner, tab, origin, edit](const fs::PathTarget& result) {
            if (!*alive)
                return;
            --operations_;
            if (generation != path_navigation_generation_ || &area() != owner || &pane() != tab ||
                current_folder() != origin || area().breadcrumb.edit_generation() != edit)
                return;
            area().breadcrumb.finish_submit(result.error);
            if (!result.error.empty())
                show_message(result.error, true);
            else if (!result.file.empty()) {
                // Reload the parent: the typed file may precede its watcher notification.
                open(result.folder);
                if (const auto shown = current_folder())
                    shown->select_name(result.file);
            } else
                go_to(result.folder, {});
            redraw_();
        });
    return {};
}
}  // namespace rummage::views
