#include "rummage/core/i18n.hpp"
#include "rummage/fs/path_completion.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
std::string Browser::navigate_path(const std::string& text) {
    auto location = fs::parse_location(text, place_sources_.home);
    const auto origin = current_folder();
    if (!location && origin && !text.empty() && text.find("://") == std::string::npos &&
        text.front() != '~' && text.find('\0') == std::string::npos)
        location = fs::Location{.folder = origin->path() / text, .uri = {}};
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
