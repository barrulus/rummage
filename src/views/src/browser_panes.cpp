#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

std::filesystem::path session_fallback() {
    // Availability is resolved by the directory worker, never while restoring the UI.
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        const std::filesystem::path path{home};
        if (path.is_absolute()) {
            return path;
        }
    }
    return "/";
}

}  // namespace

Browser::BrowsingArea::BrowsingArea(Browser& browser)
    : breadcrumb{browser.redraw_,
                 [&browser](const std::filesystem::path& folder, const std::string& child) {
                     browser.go_to(folder, child);
                 },
                 [] {
                     const char* home = std::getenv("HOME");
                     return home ? std::filesystem::path{home} : std::filesystem::path{};
                 }(),
                 &browser.pool_},
      tab_bar{browser.redraw_,
              {.select = [&browser](std::size_t index) { browser.select_tab(index); },
               .close = [&browser](std::size_t index) { browser.close_tab(index); },
               .add = [&browser] { browser.new_tab(); }}} {
    breadcrumb.set_submit_handler([&browser](std::string text) {
        auto error = browser.navigate_path(text);
        if (!error.empty())
            browser.status_bar_.show_message(error, true);
        return error;
    });
    breadcrumb.set_status_handler(
        [&browser](std::string text, bool error) { browser.show_message(std::move(text), error); });
    breadcrumb.set_copy_handler(
        [&browser](std::string text) { browser.copy_text(std::move(text)); });
}

void Browser::toggle_split() {
    if (!panes_enabled_)
        return;
    leave_pane();
    if (!areas_[1]) {
        const auto folder =
            pane().mode == Mode::details ? pane().details.root_folder() : current_folder();
        const auto path = folder && !folder->is_collection() ? folder->path() : session_fallback();
        const auto mode = pane().mode;
        const auto list = pane().details.list_size();
        const auto grid = pane().grid.size_index();
        const auto order = pane().details.order();
        const auto columns = pane().details.columns();
        const auto widths = pane().details.widths();
        const bool hidden = pane().show_hidden;
        areas_[1] = std::make_unique<BrowsingArea>(*this);
        context_area_ = active_area_ = 1;
        add_pane(0);
        pane().mode = mode;
        set_list_size(list);
        set_grid_size(grid);
        set_sort(order.key, order.descending);
        pane().details.set_columns(columns);
        pane().details.set_widths(widths);
        set_show_hidden(hidden, false);
        open(path);
    }
    split_visible_ = !split_visible_;
    button_held_ = false;
    pointer_area_ = active_area_;
    pointer_target_ = Target::view;
    sidebar_.set_focused(false);
    redraw_();
}

void Browser::focus_pane(std::size_t index) {
    if (!split_visible_ || index >= areas_.size() || !areas_[index] || index == active_area_)
        return;
    ++path_navigation_generation_;
    context_area_ = active_area_;
    note_location();
    leave_pane();
    context_area_ = active_area_ = index;
    sidebar_.set_focused(false);
    button_held_ = false;
    pointer_area_ = index;
    pointer_target_ = Target::view;
    note_location();
    if (!pane().settings_folder.empty())
        folder_settings_.remember(pane().settings_folder, folder_settings(pane()), true);
    redraw_();
}

void Browser::switch_pane() { focus_pane(1 - active_area_); }

void Browser::swap_panes() {
    if (!split_visible_)
        return;
    leave_pane();
    std::swap(areas_[0], areas_[1]);
    context_area_ = active_area_ = 1 - active_area_;
    pointer_area_ = active_area_;
    pointer_target_ = Target::view;
    button_held_ = false;
    redraw_();
}

void Browser::set_stacked(bool stacked) {
    if (stacked_ != stacked) {
        cancel_layout_motion();
    }
    stacked_ = stacked;
    redraw_();
}

void Browser::set_split_ratio(float ratio) {
    cancel_layout_motion();
    if (std::isfinite(ratio))
        split_ratio_ = std::clamp(ratio, 0.15F, 0.85F);
    redraw_();
}

std::shared_ptr<Folder> Browser::other_folder() const {
    if (!split_visible_ || !areas_[1 - active_area_])
        return {};
    const auto& other = *areas_[1 - active_area_];
    const Pane& tab = *other.tabs[other.active];
    // Expanded rows in Details do not change the destination shown by its breadcrumb.
    return tab.mode == Mode::details ? tab.details.root_folder() : folder_of(tab);
}

void Browser::open_in_other_pane(const std::filesystem::path& directory) {
    if (!panes_enabled_)
        return;
    if (!split_visible_) {
        const auto source = active_area_;
        toggle_split();
        focus_pane(1 - source);
    } else {
        switch_pane();
    }
    open_tab(directory, true);
}

void Browser::transfer_to_other_pane(bool move) {
    const auto source = current_folder();
    const auto destination = other_folder();
    if (!source || source->is_collection() ||
        (source->is_trash() || (move && source->is_archive())) || !destination ||
        (destination->is_collection() || destination->is_trash() || destination->is_archive()))
        return;
    std::vector<std::filesystem::path> paths;
    for (const auto& name : targets(*source))
        paths.push_back(source->path() / name);
    if (paths.empty())
        return;
    if (const auto why = refusal(destination->path(), paths); !why.empty()) {
        show_message(why, false);
        return;
    }
    transfer_paths(destination->path(), paths, move, std::nullopt);
}

void Browser::open_settings() {
    if (!panes_enabled_)
        return;
    menu_.close();
    palette_.close();
    filter_focused_ = false;
    if (settings_window_) {
        settings_window_();
        return;
    }
    settings_.open(config_path_.empty() ? core::default_config_path() : config_path_, open_config_,
                   settings_applied_);
}

void Browser::open_view_menu(float x, float y, bool keyboard) {
    std::vector<MenuItem> items;
    for (const auto& [command, label] : {std::pair{Command::columns_view, "Columns"},
                                         {Command::grid_view, "Grid"},
                                         {Command::details_view, "Details"}}) {
        MenuItem item;
        item.label = i18n::tr(label);
        item.keys = keys_for(command);
        item.radio = true;
        item.checked = (command == Command::columns_view && mode() == Mode::columns) ||
                       (command == Command::grid_view && mode() == Mode::grid) ||
                       (command == Command::details_view && mode() == Mode::details);
        item.action = [this, command] { run(command, Clock::now()); };
        items.push_back(std::move(item));
    }
    if (mode() == Mode::columns) {
        MenuItem root;
        root.id = "reset_column_root";
        root.label = i18n::tr("Make Focused Folder Column Root");
        root.keys = keys_for(Command::reset_column_root);
        root.action = [this] { pane().columns.reset_root(); };
        items.push_back(std::move(root));
    }
    MenuItem orientation;
    orientation.id = "vertical_columns";
    orientation.label = i18n::tr("Stack directory columns vertically");
    orientation.checkable = true;
    orientation.checked = vertical_columns_;
    orientation.action = [this] {
        vertical_columns_ = !vertical_columns_;
        set_mode(Mode::columns);
        redraw_();
    };
    items.push_back(std::move(orientation));
    MenuItem gallery;
    gallery.id = "gallery";
    gallery.label = i18n::tr("Gallery: hide media filenames");
    gallery.checkable = true;
    gallery.checked = gallery_;
    gallery.action = [this] {
        set_gallery(!gallery_);
        set_mode(Mode::grid);
    };
    items.push_back(std::move(gallery));
    items.push_back(sort_menu());
    items.push_back(name_filter_menu());
    if (!picker_mode_) {
        MenuItem results;
        results.id = "search_results";
        results.label = i18n::tr("Search Results…");
        results.action = [this] { open_search_results(); };
        items.push_back(std::move(results));
        MenuItem saved;
        saved.id = "saved_searches";
        saved.label = i18n::tr("Saved Searches…");
        saved.action = [this] { open_saved_searches(); };
        items.push_back(std::move(saved));
    }
    MenuItem verification;
    verification.id = "verify_transfers";
    verification.label = i18n::tr("Verify copied contents");
    verification.checkable = true;
    verification.checked = verifies_transfers();
    verification.action = [this] { set_verify_transfers(!verifies_transfers()); };
    items.push_back(std::move(verification));
    MenuItem transfers;
    transfers.id = "transfers";
    transfers.label = i18n::tr("Transfers…");
    transfers.action = [this] { open_transfers(); };
    items.push_back(std::move(transfers));
    MenuItem show;
    show.id = "show";
    show.label = i18n::tr("Show");
    show.submenu = true;
    MenuItem previews;
    previews.id = "show_thumbnails";
    previews.label = i18n::tr("Thumbnails");
    previews.keys = keys_for(Command::toggle_thumbnails);
    previews.checkable = true;
    previews.checked = shows_thumbnails();
    previews.action = [this] { set_show_thumbnails(!shows_thumbnails()); };
    show.children.push_back(std::move(previews));
    MenuItem extensions;
    extensions.id = "show_extensions";
    extensions.label = i18n::tr("File extensions");
    extensions.keys = keys_for(Command::toggle_extensions);
    extensions.checkable = true;
    extensions.checked = shows_extensions();
    extensions.action = [this] { set_show_extensions(!shows_extensions()); };
    show.children.push_back(std::move(extensions));
    MenuItem hidden;
    hidden.id = "show_hidden";
    hidden.label = i18n::tr("Hidden files");
    hidden.keys = keys_for(Command::toggle_hidden);
    hidden.checkable = true;
    hidden.checked = shows_hidden();
    hidden.action = [this] { set_show_hidden(!shows_hidden()); };
    show.children.push_back(std::move(hidden));
    show.children.push_back({.id = "toggle_info",
                             .label = std::string{i18n::tr("Preview")},
                             .keys = keys_for(Command::toggle_info),
                             .separator = true,
                             .checkable = true,
                             .checked = info_shown(),
                             .empty_text = {},
                             .children = {},
                             .action = [this] { set_info_visible(!info_shown()); }});
    show.children.push_back({.id = "auto_hide_info",
                             .label = std::string{i18n::tr("Hide preview when no file is focused")},
                             .keys = {},
                             .checkable = true,
                             .checked = info_auto_hide_,
                             .empty_text = {},
                             .children = {},
                             .action = [this] { set_info_auto_hide(!info_auto_hide_); }});
    show.children.push_back({.id = "toggle_sidebar",
                             .label = std::string{i18n::tr("Places sidebar")},
                             .keys = keys_for(Command::toggle_sidebar),
                             .checkable = true,
                             .checked = sidebar_visible_,
                             .empty_text = {},
                             .children = {},
                             .action = [this] { run(Command::toggle_sidebar, Clock::now()); }});
    items.push_back(std::move(show));
    MenuItem sidebar_options;
    sidebar_options.id = "sidebar_options";
    sidebar_options.label = i18n::tr("Sidebar");
    sidebar_options.submenu = true;
    sidebar_options.children = sidebar_menu();
    items.push_back(std::move(sidebar_options));
    if (mode() == Mode::details)
        items.push_back(details_columns_menu());
    auto panes = pane_items();
    if (!panes.empty()) {
        panes.front().separator = true;
        items.insert(items.end(), std::make_move_iterator(panes.begin()),
                     std::make_move_iterator(panes.end()));
    }
    if (panes_enabled_)
        items.push_back({.id = "settings",
                         .label = std::string{i18n::tr("Settings…")},
                         .keys = keys_for(Command::show_settings),
                         .separator = true,
                         .empty_text = {},
                         .children = {},
                         .action = [this] { open_settings(); }});
    menu_.open(x, y, std::move(items), keyboard);
}

std::vector<MenuItem> Browser::pane_items() {
    if (!panes_enabled_)
        return {};
    std::vector<MenuItem> items;
    const auto add = [&](std::string id, std::string label, Command command,
                         std::function<void()> action) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.keys = keys_for(command);
        item.action = std::move(action);
        items.push_back(std::move(item));
    };
    add("toggle_split", split_visible_ ? "Show Active Pane Only" : "Show Two Panes",
        Command::toggle_split, [this] { toggle_split(); });
    if (split_visible_) {
        add("switch_pane", "Switch Pane", Command::switch_pane, [this] { switch_pane(); });
        add("swap_panes", "Swap Panes", Command::swap_panes, [this] { swap_panes(); });
        add("orient_panes", stacked_ ? "Arrange Side by Side" : "Arrange Top / Bottom",
            Command::orient_panes, [this] { set_stacked(!stacked_); });
    }
    const auto source = current_folder();
    if (!source || source->is_collection() || source->is_trash())
        return items;
    const auto same = pane().mode == Mode::details ? pane().details.root_folder() : source;
    if (same)
        add("same_other_pane", "Show Same Folder in Other Pane", Command::same_other_pane,
            [this, path = same->path()] { open_in_other_pane(path); });
    const auto names = targets(*source);
    if (names.size() == 1) {
        const auto entry = std::ranges::find(source->entries(), names.front(), &fs::Entry::name);
        if (entry != source->entries().end() && entry->is_directory()) {
            add("open_other_pane", "Open in Other Pane", Command::open_other_pane,
                [this, path = source->path() / entry->name] { open_in_other_pane(path); });
        }
    }
    const auto destination = other_folder();
    if (!destination ||
        (destination->is_collection() || destination->is_trash() || destination->is_archive()) ||
        names.empty())
        return items;
    std::vector<std::filesystem::path> paths;
    for (const auto& name : names)
        paths.push_back(source->path() / name);
    if (!refusal(destination->path(), paths).empty())
        return items;
    const std::string direction =
        stacked_ ? (active_area_ == 0 ? "↓" : "↑") : (active_area_ == 0 ? "→" : "←");
    for (const bool move : {false, true}) {
        if (move && source->is_archive())
            continue;
        add(move ? "move_other_pane" : "copy_other_pane",
            std::format("{} to “{}” {}", move ? "Move" : "Copy", destination->path().string(),
                        direction),
            move ? Command::move_other_pane : Command::copy_other_pane,
            [this, path = destination->path(), paths, move] {
                transfer_paths(path, paths, move, std::nullopt);
            });
    }
    return items;
}

bool Browser::restore_session(const State& state) {
    if (!panes_enabled_)
        return restore_tabs(state.tabs, state.shown_tab);
    std::array<std::vector<State::Tab>, 2> tabs;
    for (const auto& tab : state.tabs) {
        if (tab.pane < tabs.size() && tabs[tab.pane].size() < State::kMaxTabs)
            tabs[tab.pane].push_back(tab);
    }
    context_area_ = active_area_ = 0;
    bool restored = restore_tabs(tabs[0], state.shown_tab);
    if (state.panes_initialized || !tabs[1].empty()) {
        areas_[1] = std::make_unique<BrowsingArea>(*this);
        context_area_ = 1;
        add_pane(0);
        apply(state);
        const bool second = restore_tabs(tabs[1], state.other_shown_tab);
        if (!second) {
            open(session_fallback());
        }
        if (!restored) {
            context_area_ = 0;
            open(session_fallback());
        }
        restored = true;
        split_visible_ = state.split_visible;
        active_area_ = std::min(state.active_pane, std::size_t{1});
    }
    stacked_ = state.stacked;
    set_split_ratio(state.split_ratio);
    context_area_ = pointer_area_ = active_area_;
    return restored;
}

std::optional<std::size_t> Browser::area_at(float x, float y) const {
    for (std::size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i] && (split_visible_ || i == active_area_) && areas_[i]->bounds.contains(x, y))
            return i;
    }
    return {};
}

}  // namespace rummage::views
