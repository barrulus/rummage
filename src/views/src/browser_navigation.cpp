#include <algorithm>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/path_completion.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/fuzzy.hpp"

namespace rummage::views {
void Browser::clear_type_jump() {
    loop_.remove(type_jump_timer_);
    type_jump_timer_ = 0;
    type_jump_prefix_.clear();
    type_jump_folder_.reset();
    status_bar_.set_type_jump({});
}

bool Browser::match_type_jump() {
    if (pane().mode == Mode::details)
        return pane().details.jump_to_prefix(type_jump_prefix_);
    const auto folder = current_folder();
    if (!folder)
        return false;
    const auto& entries = folder->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (filename_prefix_matches(entries[i].name, type_jump_prefix_)) {
            folder->set_cursor(i, SelectMode::replace);
            return true;
        }
    }
    return false;
}

bool Browser::type_jump(const ui::KeyEvent& event, Clock::time_point now) {
    if (!keyboard_config_.type_to_jump || event.modifiers.ctrl || event.modifiers.alt ||
        event.modifiers.super || pane().insights.is_open())
        return false;
    const auto context =
        pane().mode == Mode::details ? pane().details.root_folder() : current_folder();
    if (!context)
        return false;
    if (now >= type_jump_until_ || type_jump_folder_.lock() != context ||
        type_jump_mode_ != pane().mode)
        clear_type_jump();
    if (event.keysym == XKB_KEY_Escape) {
        if (type_jump_prefix_.empty())
            return false;
        clear_type_jump();
        return true;
    }
    if (event.keysym == XKB_KEY_BackSpace) {
        if (type_jump_prefix_.empty())
            return false;
        auto end = type_jump_prefix_.size() - 1;
        while (end > 0 && (static_cast<unsigned char>(type_jump_prefix_[end]) & 0xc0U) == 0x80U)
            --end;
        type_jump_prefix_.resize(end);
        if (type_jump_prefix_.empty()) {
            clear_type_jump();
            return true;
        }
    } else {
        if (event.text.empty() || type_jump_prefix_.size() + event.text.size() > 256 ||
            std::ranges::any_of(event.text, [](unsigned char c) { return c < 32 || c == 127; }) ||
            !filename_prefix_matches(event.text, event.text))
            return false;
        type_jump_prefix_ += event.text;
    }
    type_jump_folder_ = context;
    type_jump_mode_ = pane().mode;
    const auto timeout = std::chrono::milliseconds{1000};
    type_jump_until_ = now + timeout;
    loop_.remove(type_jump_timer_);
    type_jump_timer_ = loop_.add_timer(timeout, std::chrono::milliseconds{0}, [this] {
        type_jump_timer_ = 0;
        clear_type_jump();
    });
    info_pane_.clear_text_focus();
    status_bar_.set_type_jump(type_jump_prefix_, match_type_jump());
    redraw_();
    return true;
}

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
    const auto folder = pane().insights.is_open()      ? pane().insights.scope_folder()
                        : pane().mode == Mode::details ? pane().details.root_folder()
                                                       : current_folder();
    if (folder)
        area().breadcrumb.edit(folder->path());
}

void Browser::open_selected_tab() {
    if (showing_photos()) {
        open_selection();
        return;
    }
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
    if (!location && base && !base->is_collection() && !text.empty() &&
        text.find("://") == std::string::npos && text.front() != '~' &&
        text.find('\0') == std::string::npos)
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
