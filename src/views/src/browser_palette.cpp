// The command palette's items (ROADMAP 8a): what Browser offers through Ctrl+K.

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/name_pattern.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/command_palette.hpp"
#include "rummage/views/file_clipboard.hpp"
#include "rummage/views/format.hpp"
#include "rummage/views/fuzzy.hpp"
#include "rummage/views/shortcuts.hpp"

namespace rummage::views {
namespace {

// The keys shown for a command: its shortcut line's first chord ("Alt+←" of "Alt+←  Alt+→").
std::string first_chord(Command command) {
    std::string keys = keys_for(command);
    if (const std::size_t gap = keys.find("  "); gap != std::string::npos) {
        keys.resize(gap);
    }
    return keys;
}

}  // namespace

void Browser::open_palette() {
    palette_use_ = PaletteUse::commands;
    menu_.close();
    shortcuts_.close();
    palette_.open(palette_items(), palette_recent_,
                  std::string{i18n::tr("Type a command, a place or a tab")});
}

std::vector<PaletteItem> Browser::palette_items() {
    std::vector<PaletteItem> items;
    const auto add = [&items](std::string id, std::string label, std::string keys,
                              std::function<void()> action, bool remember = true,
                              bool translate = true) {
        const auto shown =
            !translate
                ? label
                : std::string{label == "Open" ? i18n::trc("file-action", "Open") : i18n::tr(label)};
        items.push_back({.id = std::move(id),
                         .label = shown,
                         .keys = std::move(keys),
                         .remember = remember,
                         .action = std::move(action),
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    };
    // A command goes the way its key goes: through run().
    const auto command = [&](Command which, const char* id, std::string label) {
        add(std::string{"command:"} + id, std::move(label), first_chord(which),
            [this, which] { run(which, Clock::now()); });
    };
    if (photos_available()) {
        add("photos:open", std::string{i18n::tr("Photo Library")}, "",
            [this] { open_photo_library(); });
        const auto folder = current_folder();
        if (showing_photos())
            add("photos:add", std::string{i18n::tr("Add folder to Photo Library…")}, "",
                [this] { photo_library_->add_folder(); });
        else if (folder && !folder->is_collection() && !folder->is_trash() && !folder->is_archive())
            add("photos:add", std::string{i18n::tr("Add folder to Photo Library…")}, "",
                [this, path = folder->path()] { open_photo_library(path); });
    }
    if (showing_photos()) {
        add("photos:close", std::string{i18n::tr("Close Photo Library")},
            keys_for(Command::close_tab), [this] {
                photo_library_->close();
                if (area().tabs.size() > 1)
                    close_tab(area().active);
                else
                    open(place_sources_.home.is_absolute() ? place_sources_.home
                                                           : std::filesystem::path{"/"});
            });
        return items;
    }
    if (searching_results()) {
        using Action = SearchResults::Action;
        const auto count = pane().search_results->chosen().size();
        const auto offer = [&](const char* id, std::string_view label, Action action,
                               bool enabled = true) {
            if (enabled)
                add(id, std::string{label}, "", [this, action] { search_action(action); }, false);
        };
        offer("search:copy", i18n::mark("Copy"), Action::copy, count > 0);
        offer("search:cut", i18n::mark("Cut"), Action::cut, count > 0);
        offer("search:trash", i18n::mark("Move to Trash"), Action::trash, count > 0);
        offer("search:rename", i18n::mark("Rename…"), Action::rename, count == 1);
        offer("search:preview", i18n::mark("Quick Look"), Action::preview, count == 1);
        offer("search:reveal", i18n::mark("Show in Folder"), Action::reveal, count == 1);
        offer("search:save", i18n::mark("Save Search…"), Action::save);
        offer("search:saved", i18n::mark("Saved Searches…"), Action::saved);
        offer("search:details", i18n::mark("Copy Search Details"), Action::details);
        offer("search:close", i18n::mark("Close"), Action::close);
        return items;
    }
    // A key the view handles itself (zoom, select all, parent): the same key, sent to it.
    const auto view_command = [&](const char* id, std::string label, std::string keys,
                                  xkb_keysym_t keysym, ui::Modifiers modifiers) {
        add(std::string{"view:"} + id, std::move(label), std::move(keys),
            [this, keysym, modifiers] {
                ui::KeyEvent event;
                event.keysym = keysym;
                event.modifiers = modifiers;
                browse_key(event, Clock::now());
            });
    };

    add("sidebar:restore", std::string{i18n::mark("Restore Hidden Places")}, "",
        [this] { sidebar_.restore_places(); });
    if (sidebar_.focused()) {
        if (const auto* place = sidebar_.cursor_place()) {
            for (auto& item : place_navigation_items(*place)) {
                if (item.enabled && item.action)
                    add("place:" + item.id, item.label, item.keys, std::move(item.action), false,
                        false);
            }
        }
        command(Command::edit_path, "edit_path", std::string{i18n::mark("Edit Path")});
        command(Command::focus_files, "focus_files", std::string{i18n::mark("Focus Files")});
        command(Command::go_to, "go_to", "Go to a folder or a server");
        if (panes_enabled_ && split_visible_) {
            command(Command::focus_first_pane, "focus_first_pane",
                    std::string{i18n::mark("Focus First Pane")});
            command(Command::focus_second_pane, "focus_second_pane",
                    std::string{i18n::mark("Focus Second Pane")});
        }
        return items;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    const bool in_trash = folder && folder->is_trash();
    const std::vector<std::string> names = folder ? targets(*folder) : std::vector<std::string>{};
    const std::unordered_set<std::string> target_names(names.begin(), names.end());
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    const bool editable = folder && !in_trash && !folder->is_archive();
    if (folder && !folder->is_archive() && (!in_trash || !names.empty()))
        command(Command::properties, "properties", "Properties…");

    if (editable && !picker_mode_) {
        command(Command::folder_insights, "folder_insights", "Insights…");
        for (auto& item : insights_menu()) {
            if (item.action)
                add("command:" + item.id, std::format("{} · {}", i18n::tr("Insights"), item.label),
                    "", std::move(item.action), true, false);
            for (auto& child : item.children)
                add("command:" + child.id,
                    std::format("{} · {} {}", i18n::tr("Insights"), item.label, child.label), "",
                    std::move(child.action), true, false);
        }
    }

    if (editable && !names.empty()) {
        if (!picker_mode_)
            add("command:move_into_folder", std::string{i18n::mark("Move into New Folder…")}, "",
                [this] { move_into_new_folder(); });
        if (!picker_mode_)
            for (auto& item : link_menu(false))
                add("command:" + item.id, item.label, "", std::move(item.action), true, false);
        add("command:compress_zip", "Compress to ZIP…", "", [this] { compress(false); });
        add("command:compress_tar", "Compress to tar.gz…", "", [this] { compress(true); });
    }
    if (editable && names.size() == 1 && cursor && cursor->is_link)
        add("command:reveal_link_target", std::string{i18n::mark("Go to Link Target")}, "",
            [this] { reveal_link_target(); });
    command(Command::edit_path, "edit_path", std::string{i18n::mark("Edit Path")});
    command(Command::focus_files, "focus_files", std::string{i18n::mark("Focus Files")});
    command(Command::focus_places, "focus_places", std::string{i18n::mark("Focus Places")});
    if (panes_enabled_ && split_visible_) {
        command(Command::focus_first_pane, "focus_first_pane",
                std::string{i18n::mark("Focus First Pane")});
        command(Command::focus_second_pane, "focus_second_pane",
                std::string{i18n::mark("Focus Second Pane")});
    }
    if (folder) {
        command(Command::expand_folder, "expand_folder", std::string{i18n::mark("Expand Folder")});
        command(Command::collapse_folder, "collapse_folder",
                std::string{i18n::mark("Collapse Folder")});
    }
    // Opening and moving.
    if (cursor != nullptr)
        command(Command::open_selected, "open", "Open");
    if (cursor != nullptr && editable && choices_ && open_with_) {
        add("command:open_with", "Open With…", "", [this] { open_with_palette(); });
    }
    if (panes_enabled_ && !picker_mode_ && folder && !in_trash && !names.empty()) {
        std::vector<std::filesystem::path> folders;
        for (const fs::Entry& entry : folder->entries()) {
            if (entry.is_directory() && target_names.contains(entry.name)) {
                folders.push_back(folder->path() / entry.name);
            }
        }
        if (!folders.empty() && folders.size() == names.size()) {
            add("command:open_tab", folders.size() == 1 ? "Open in New Tab" : "Open in New Tabs",
                keys_for(Command::open_selected_tab),
                [this] { run(Command::open_selected_tab, Clock::now()); });
        }
    }
    if (editable && terminal_) {
        command(Command::open_terminal, "open_terminal", "Open in Terminal");
    }
    if (folder) {
        command(Command::copy_path, "copy_path", "Copy Path");
    }
    if (folder && folder->path().has_parent_path() &&
        folder->path() != folder->path().root_path()) {
        view_command("parent", "Go to Parent Folder", keys_for(Command::parent_folder), XKB_KEY_Up,
                     {.alt = true});
    }
    if (pane().here > 0) {
        command(Command::back, "back", "Go Back");
    }
    if (pane().here + 1 < pane().history.size()) {
        add("command:forward", "Go Forward", "Alt+→",
            [this] { run(Command::forward, Clock::now()); });
    }

    // Files.
    if (editable) {
        command(Command::new_folder, "new_folder", "New Folder");
        add("command:new_file", "New Empty File", "",
            [this] { new_file(std::string{i18n::tr("New File")}, {}); });
    }
    if (!names.empty() && !in_trash) {
        command(Command::copy, "copy", "Copy");
        if (!folder->is_archive())
            command(Command::cut, "cut", "Cut");
    }
    bool can_paste = !clipboard_.names.empty() || !clipboard_.paths.empty();
    if (!can_paste && system_.types) {
        const std::vector<std::string> types = system_.types();
        can_paste = std::ranges::find(types, kGnomeFiles) != types.end() ||
                    std::ranges::find(types, kUriList) != types.end();
    }
    if (editable && can_paste) {
        command(Command::paste, "paste", "Paste");
        if (!picker_mode_)
            for (auto& item : link_menu(true))
                add("command:" + item.id, item.label, "", std::move(item.action), true, false);
    }
    if (editable && !names.empty() && (names.size() == 1 || !picker_mode_)) {
        command(Command::rename, "rename", names.size() > 1 ? "Batch Rename…" : "Rename…");
    }
    if (folder && !folder->is_archive()) {
        if (const RunningTargets running = running_targets(*folder); !running.files.empty()) {
            const bool stop = running.all_run;
            add("command:running", stop ? "Stop Running as Program" : "Allow Running as Program",
                "", [this, stop] { set_running(!stop); });
        }
    }
    if (!names.empty() && !in_trash && !folder->is_archive()) {
        command(Command::trash, "trash", "Move to Trash");
    }
    if (!names.empty() && !folder->is_archive()) {
        command(Command::erase, "erase", "Delete Permanently…");
    }
    if (!names.empty() && in_trash) {
        add("command:restore", "Restore", "", [this] { restore(); });
    }
    add("command:empty_trash", "Empty Trash…", "", [this] { empty_trash(); });
    if (undo_) {
        command(Command::undo, "undo", "Undo");
    }
    if (redo_) {
        command(Command::redo, "redo", "Redo");
    }
    if (transfers_.pending() > 0) {
        command(Command::stop_transfer, "stop_transfer", "Stop the Transfer");
    }
    add("command:transfers", std::string{i18n::mark("Transfers…")}, "",
        [this] { open_transfers(); });
    add("command:verify_transfers",
        std::string{verifies_transfers() ? i18n::mark("Disable Copy Verification")
                                         : i18n::mark("Enable Copy Verification")},
        "", [this] { set_verify_transfers(!verifies_transfers()); });
    if (folder) {
        view_command("select_all", "Select All", keys_for_binding("select_all"), XKB_KEY_a,
                     {.ctrl = true});
        add("view:invert_selection", std::string{i18n::mark("Invert Selection")},
            keys_for_binding("invert_selection"), [this] {
                if (const auto shown = current_folder()) {
                    shown->invert_selection();
                    redraw_();
                }
            });
    }
    if (editable) {
        command(Command::add_bookmark, "add_bookmark", "Add to Sidebar");
    }
    if (has_custom_folder_view())
        command(Command::reset_folder_view, "reset_folder_view",
                "Use Default View for This Folder");
    if (folder && pattern_selection_enabled_)
        command(Command::select_pattern, "select_pattern",
                std::string{i18n::mark("Select by Pattern…")});
    command(Command::go_to, "go_to", "Go to Folder or Server…");

    // The same registry entries and captured targets as the context menu.
    for (auto& item : action_items(action_context(folder))) {
        if (item.submenu) {
            for (auto& child : item.children) {
                if (child.enabled) {
                    add(child.id, child.label, child.keys, std::move(child.action), true, false);
                }
            }
        } else if (item.enabled) {
            add(item.id, item.label, item.keys, std::move(item.action), true, false);
        }
    }

    for (auto& item : pane_items()) {
        add("command:" + item.id, item.label, item.keys, std::move(item.action), true, false);
    }

    if (panes_enabled_)
        command(Command::show_settings, "show_settings", "Settings…");

    // Views.
    if (pane().mode == Mode::columns)
        command(Command::reset_column_root, "reset_column_root", "Make Focused Folder Column Root");
    if (pane().mode != Mode::columns) {
        command(Command::columns_view, "columns_view", "Show Columns");
    }
    if (pane().mode != Mode::grid) {
        command(Command::grid_view, "grid_view", "Show Grid");
    }
    if (pane().mode != Mode::details) {
        command(Command::details_view, "details_view", "Show Details");
    }
    if (cursor != nullptr || quick_look_.is_open()) {
        command(Command::quick_look, "quick_look",
                quick_look_.is_open() ? "Close Quick Look" : "Quick Look");
    }
    if (split_visible_ || pane().mode != Mode::columns) {  // shared preview in a split
        command(Command::toggle_info, "toggle_info",
                info_visible_ ? "Hide Info Pane" : "Show Info Pane");
    }
    command(Command::toggle_sidebar, "toggle_sidebar",
            sidebar_visible_ ? "Hide Sidebar" : "Show Sidebar");
    command(Command::focus_sidebar, "focus_sidebar",
            sidebar_.focused() ? "Back to the View" : "Move to the Sidebar");
    command(Command::toggle_thumbnails, "toggle_thumbnails",
            shows_thumbnails() ? "Hide Thumbnails" : "Show Thumbnails");
    command(Command::toggle_extensions, "toggle_extensions",
            shows_extensions() ? "Hide File Extensions" : "Show File Extensions");
    command(Command::toggle_hidden, "toggle_hidden",
            pane().show_hidden ? "Hide Hidden Files" : "Show Hidden Files");
    const fs::SortOrder& order = sort_order();

    for (const auto& column : kDetailsColumns) {
        if (order.key != column.field) {
            add("sort:" + std::string{column.id}, i18n::trf("Sort by {}", i18n::tr(column.title)),
                "", [this, key = column.field] { set_sort(key, sort_order().descending); });
        }
        if (pane().mode == Mode::details && column.field != DetailsView::Field::name) {
            add("column:" + std::string{column.id},
                pane().details.column_visible(column.field)
                    ? i18n::trf("Hide {} column", i18n::tr(column.title))
                    : i18n::trf("Show {} column", i18n::tr(column.title)),
                "", [this, field = column.field] {
                    pane().details.set_column_visible(field, !pane().details.column_visible(field));
                });
        }
    }
    if (order.descending) {
        add("sort:ascending", "Sort Ascending", "", [this] { set_sort(sort_order().key, false); });
    } else {
        add("sort:descending", "Sort Descending", "", [this] { set_sort(sort_order().key, true); });
    }
    if (folder) {
        view_command("zoom_in", "Zoom In", "Ctrl++", XKB_KEY_plus, {.ctrl = true});
        view_command("zoom_out", "Zoom Out", "Ctrl+−", XKB_KEY_minus, {.ctrl = true});
        view_command("zoom_reset", "Usual Size", "Ctrl+0", XKB_KEY_0, {.ctrl = true});
    }

    if (!picker_mode_) {
        command(Command::toggle_terminal, "toggle_terminal",
                std::string{i18n::mark("Toggle Terminal")});
        command(Command::focus_terminal, "focus_terminal",
                std::string{i18n::mark("Focus Terminal or Files")});
        if (terminal_workflow_available(1))
            command(Command::fzf_navigate, "fzf_navigate",
                    std::string{i18n::mark("Navigate with fzf…")});
        if (terminal_workflow_available(2))
            command(Command::zoxide_jump, "zoxide_jump",
                    std::string{i18n::mark("Jump with zoxide…")});
        command(Command::shell_command, "shell_command",
                std::string{i18n::mark("Run Shell Command…")});
    }
    // Tabs.
    command(Command::new_tab, "new_tab", "New Tab");
    if (!picker_mode_ && new_window_)
        command(Command::new_window, "new_window", std::string{i18n::mark("New Window")});
    if (!area().closed_tabs.empty())
        command(Command::reopen_tab, "reopen_tab", std::string{i18n::mark("Reopen Closed Tab")});
    if (area().tabs.size() > 1) {
        command(Command::close_tab, "close_tab", "Close Tab");
        command(Command::next_tab, "next_tab", "Next Tab");
        command(Command::previous_tab, "previous_tab", "Previous Tab");
        const std::vector<std::string> titles = tab_titles();
        for (std::size_t i = 0; i < area().tabs.size(); ++i) {
            if (i == area().active) {
                continue;
            }
            add(
                std::format("tab:{}", i), i18n::trf("Tab {}: {}", i + 1, titles[i]),
                i < 9 ? std::format("Alt+{}", i + 1) : std::string{}, [this, i] { select_tab(i); },
                false);
        }
    }

    // Places, as the sidebar lists them.
    for (const fs::Place& place : places_) {
        const std::string where = place.path.empty() ? place.disk.volume : place.path.string();
        add("go:" + where, i18n::trf("Go to {}", place.label), "",
            [this, place] { open_place(place); });
        if (place.section == fs::Place::Section::devices && place.disk.mounted &&
            !place.disk.volume.empty()) {
            add("eject:" + place.disk.volume,
                place.disk.ejectable ? i18n::trf("Eject {}", place.label)
                                     : i18n::trf("Unmount {}", place.label),
                "", [this, place] {
                    if (place.disk.ejectable) {
                        eject_place(place);
                    } else {
                        unmount_place(place);
                    }
                });
        }
    }

    if (!picker_mode_) {
        add("command:search_results", std::string{i18n::mark("Search Results…")}, "",
            [this] { open_search_results(); });
        add("command:saved_searches", std::string{i18n::mark("Saved Searches…")}, "",
            [this] { open_saved_searches(); });
    }
    // rummage.
    if (folder && !in_trash) {
        command(Command::filter_names, "filter_names", "Filter Names…");
        command(Command::find_files, "find_files", "Find Files…");
        add("command:find_contents", std::string{i18n::mark("Find File Contents…")}, "", [this] {
            open_finder();
            if (finder_)
                palette_.set_query("content: ");
        });
    }
    command(Command::show_shortcuts, "show_shortcuts", "Keyboard Shortcuts");
    if (quit_) {
        add("command:quit", "Quit", "Ctrl+Q", [this] { quit_(); });
    }
    return items;
}

namespace {

std::string task_state(fs::TransferState state) {
    switch (state) {
        case fs::TransferState::queued:
            return std::string{i18n::tr("Queued")};
        case fs::TransferState::running:
            return std::string{i18n::tr("Running")};
        case fs::TransferState::paused:
            return std::string{i18n::tr("Paused")};
        case fs::TransferState::waiting:
            return std::string{i18n::tr("Waiting for an answer")};
        case fs::TransferState::completed:
            return std::string{i18n::tr("Completed")};
        case fs::TransferState::failed:
            return std::string{i18n::tr("Failed")};
        case fs::TransferState::cancelled:
            return std::string{i18n::tr("Cancelled")};
    }
    return {};
}

std::string task_kind(const fs::TransferRequest& request) {
    switch (request.kind) {
        case fs::TransferKind::copy:
            return std::string{request.from_archive ? i18n::tr("Extract") : i18n::tr("Copy")};
        case fs::TransferKind::move:
            return std::string{i18n::tr("Move")};
        case fs::TransferKind::trash:
            return std::string{i18n::tr("Move to Trash")};
        case fs::TransferKind::restore:
            return std::string{i18n::tr("Restore")};
        case fs::TransferKind::erase:
            return std::string{i18n::tr("Delete Permanently…")};
        case fs::TransferKind::group:
            return std::string{i18n::tr("Move into New Folder…")};
        case fs::TransferKind::link:
            return std::string{i18n::tr("Create Link")};
        case fs::TransferKind::compress:
            return std::string{i18n::tr("Compress")};
        case fs::TransferKind::empty_trash:
            return std::string{i18n::tr("Empty Trash…")};
    }
    return {};
}

std::string task_progress(const fs::TransferTask& task) {
    const auto& p = task.progress;
    if (!task.started)
        return task_state(task.state);
    if (p.preparing && !task.finished())
        return std::string{i18n::tr("Adding up the size…")};
    if (task.request.verify)
        return i18n::trf("Copied {} of {} · verified {}", format_size(p.bytes_done),
                         format_size(p.bytes_total), format_size(p.bytes_verified));
    return i18n::trf("Items: {} / {} · {} / {}", p.files_done, p.files_total,
                     format_size(p.bytes_done), format_size(p.bytes_total));
}

}  // namespace

void Browser::open_transfers() {
    palette_.close();
    menu_.close();
    shortcuts_.close();
    quick_look_.close();
    transfer_task_id_.reset();
    palette_use_ = PaletteUse::transfers;
    palette_.open_external(std::string{i18n::tr("Search transfers")},
                           [this](const std::string&) { refresh_transfer_tasks(); });
    refresh_transfer_tasks();
}

void Browser::open_transfer_task(std::uint64_t id) {
    open_transfers();
    transfer_task_id_ = id;
    refresh_transfer_tasks();
}

void Browser::refresh_transfer_tasks() {
    if (palette_use_ != PaletteUse::transfers || !palette_.external())
        return;
    const auto tasks = transfers_.tasks();
    std::vector<PaletteItem> items;
    FuzzyQuery query{palette_.query()};
    const auto add = [&](std::string id, std::string label, std::function<void()> action = {},
                         std::string search = {}) {
        if (!query.score(label + " " + search))
            return;
        items.push_back({.id = std::move(id),
                         .label = std::move(label),
                         .keys = {},
                         .remember = false,
                         .action = std::move(action),
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    };
    if (!transfer_task_id_) {
        for (const auto& task : tasks) {
            const auto& r = task.request;
            const std::string name =
                r.names.empty() ? r.source.filename().string() : r.names.front();
            std::string search = r.source.string() + " " + r.destination.string();
            for (const auto& n : r.names)
                search += " " + n;
            add(
                std::format("task:{}", task.id),
                std::format("#{} · {} · {} · {} · {}", task.id, task_state(task.state),
                            task_kind(r), name, task_progress(task)),
                [this, id = task.id] { open_transfer_task(id); }, std::move(search));
        }
        if (std::ranges::any_of(tasks, &fs::TransferTask::finished)) {
            add("tasks:clear", std::string{i18n::tr("Clear Finished Tasks")}, [this] {
                transfers_.clear_finished();
                open_transfers();
            });
        }
        palette_.set_results(std::move(items), tasks.empty()
                                                   ? std::string{i18n::tr("No transfers yet")}
                                                   : i18n::trf("Tasks: {} · pending: {}",
                                                               tasks.size(), transfers_.pending()));
        return;
    }
    const auto id = *transfer_task_id_;
    add("tasks:back", std::string{i18n::tr("Back to Transfers")}, [this] { open_transfers(); });
    const auto found = std::ranges::find(tasks, id, &fs::TransferTask::id);
    if (found == tasks.end()) {
        palette_.set_results(std::move(items),
                             std::string{i18n::tr("This task is no longer in history")});
        return;
    }
    const auto& task = *found;
    if (!task.finished()) {
        if (task.pausable) {
            const bool paused = task.state == fs::TransferState::paused;
            add("task:pause",
                std::string{paused ? i18n::tr("Resume Task") : i18n::tr("Pause Task")},
                [this, id, paused] {
                    transfers_.pause(id, !paused);
                    copy_started_.reset();
                    open_transfer_task(id);
                });
        } else {
            add("task:unsupported",
                std::string{i18n::tr("Pause is unavailable for this operation")});
        }
        add("task:cancel", std::string{i18n::tr("Cancel Task")}, [this, id] {
            cancel_transfer_task(id);
            open_transfer_task(id);
        });
    }
    const std::string heading =
        std::format("#{} · {} · {}", id, task_kind(task.request), task_state(task.state));
    std::string details = heading + "\n" + task_progress(task);
    const auto info = [&](std::string key, std::string text) {
        details += "\n" + text;
        add(std::move(key), std::move(text));
    };
    add("task:progress", task_progress(task));
    info("task:source", i18n::trf("Source: {}", task.request.source.string()));
    if (!task.request.destination.empty())
        info("task:destination", i18n::trf("Destination: {}", task.request.destination.string()));
    for (std::size_t i = 0; i < task.request.names.size() && i < 64; ++i)
        info(std::format("task:name:{}", i), task.request.names[i]);
    if (task.request.names.size() > 64)
        info("task:more", i18n::trf("Additional items: {}", task.request.names.size() - 64));
    for (std::size_t i = 0; i < task.errors.size(); ++i)
        info(std::format("task:error:{}", i), task.errors[i]);
    if (task.error_count > task.errors.size())
        info("task:more-errors",
             i18n::trf("Additional errors: {}", task.error_count - task.errors.size()));
    add("task:copy-details", std::string{i18n::tr("Copy Task Details")},
        [this, id, details = std::move(details)] {
            copy_text(details);
            open_transfer_task(id);
        });
    palette_.set_results(std::move(items), heading);
}

void Browser::open_with_palette() {
    if (showing_photos())
        return;
    const std::shared_ptr<Folder> folder = current_folder();
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    if (cursor == nullptr || !choices_ || !open_with_) {
        return;
    }
    // What it would open: the selected entries of the cursor's kind, else the cursor's (as the
    // context menu's "Open With").
    const std::vector<std::string> names = targets(*folder);
    const std::unordered_set<std::string> target_names(names.begin(), names.end());
    std::vector<fs::Entry> files;
    for (const fs::Entry& entry : folder->entries()) {
        if (entry.is_directory() == cursor->is_directory() && target_names.contains(entry.name)) {
            fs::Entry typed = entry;
            typed.mime = folder->mime_of(entry);
            files.push_back(std::move(typed));
        }
    }
    fs::Entry typed = *cursor;
    typed.mime = folder->mime_of(*cursor);
    ++menu_loads_;
    (all_choices_ ? all_choices_ : choices_)(
        folder->path(), std::move(typed),
        [this, alive = alive_, directory = folder->path(),
         files = std::move(files)](const std::vector<AppChoice>& apps) {
            if (!*alive) {
                return;
            }
            --menu_loads_;
            if (palette_.is_open() || menu_.is_open()) {
                return;  // something else meanwhile
            }
            std::vector<PaletteItem> items;
            items.reserve(apps.size());
            for (const AppChoice& app : apps) {
                items.push_back(
                    {.id = "open_with:" + app.id,
                     .label =
                         app.is_default ? app.name + std::string{i18n::tr(" (default)")} : app.name,
                     .keys = {},
                     .remember = false,
                     .action =
                         [this, directory, files, id = app.id, name = app.name] {
                             if (!make_default_) {
                                 open_with_(directory, files, id);
                                 return;
                             }
                             std::vector<PaletteItem> options;
                             options.push_back(
                                 {.id = "application:once",
                                  .label = std::string{i18n::tr("Open once")},
                                  .keys = {},
                                  .remember = false,
                                  .action = [this, directory, files,
                                             id] { open_with_(directory, files, id); },
                                  .path = false,
                                  .quick_look = {},
                                  .open = {}});
                             const bool same_type =
                                 !files.empty() && files.front().mime != fs::kUnknownMime &&
                                 std::ranges::all_of(files, [&](const auto& entry) {
                                     return entry.mime == files.front().mime;
                                 });
                             if (same_type)
                                 options.push_back(
                                     {.id = "application:default",
                                      .label = std::string{i18n::tr("Set as default and open")},
                                      .keys = {},
                                      .remember = false,
                                      .action = [this, directory, files,
                                                 id] { make_default_(directory, files, id); },
                                      .path = false,
                                      .quick_look = {},
                                      .open = {}});
                             palette_.open(std::move(options), {}, name);
                         },
                     .path = false,
                     .quick_look = {},
                     .open = {}});
            }
            if (items.empty()) {
                show_message(std::string{i18n::tr("No applications open it")}, false);
                return;
            }
            palette_.open(std::move(items), {}, std::string{i18n::tr("Open with…")});
        });
}

void Browser::open_finder() {
    if (showing_photos()) {
        filter_focused_ = true;
        pane().filter_field.select_all();
        redraw_();
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (folder->is_trash()) {
        show_message(std::string{i18n::tr("Files are not looked for in the trash")}, false);
        return;
    }
    if (folder->is_archive()) {
        show_message(std::string{i18n::tr("Files are not looked for inside an archive")}, false);
        return;
    }
    menu_.close();
    shortcuts_.close();
    palette_.close();
    palette_use_ = PaletteUse::files;
    finder_ = std::make_unique<FileFinder>(pool_, [this] { refresh_found(); });
    const std::filesystem::path root = folder->path();
    std::string name = root.filename().string();
    if (name.empty()) {
        name = root.string();  // "/"
    }
    palette_.open_external(i18n::trf("Find in {}", name), [this](const std::string& query) {
        if (finder_) {
            finder_->set_query(query);
        }
    });
    finder_->start(root, pane().show_hidden);
    refresh_found();
}

void Browser::open_pattern() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (!pattern_selection_enabled_) {
        show_message(std::string{i18n::tr("Nothing is selected by name while saving")}, false);
        return;
    }
    quick_look_.close();  // the cursor may move: Quick Look would no longer show it
    menu_.close();
    shortcuts_.close();
    palette_.close();
    palette_use_ = PaletteUse::pattern;
    palette_.open_external(std::string{i18n::tr("Select by name: text:, glob:, re:, case:")},
                           [this](const std::string& typed) { refresh_pattern(typed); });
    refresh_pattern({});
}

void Browser::refresh_pattern(const std::string& typed) {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || palette_use_ != PaletteUse::pattern) {
        return;
    }
    const std::string glob = typed;
    if (glob.empty()) {
        palette_.set_results(
            {}, std::string{i18n::tr("Examples: glob:*.ORF, re:^IMG_[0-9]+, case:text:Photo")});
        return;
    }
    const fs::NamePattern pattern{glob};
    if (!pattern.error().empty()) {
        palette_.set_results({}, pattern.error());
        return;
    }
    const std::vector<std::size_t> rows = folder->matching(glob);
    const std::size_t shown = folder->entries().size();
    if (rows.empty()) {
        palette_.set_results({}, i18n::trf("None of {} match", shown));
        return;
    }
    std::vector<PaletteItem> items;
    const auto row = [&](std::string id, std::string label, bool add) {
        items.push_back({.id = std::move(id),
                         .label = std::move(label),
                         .keys = {},
                         .remember = false,
                         .action = [this, glob, add] { select_pattern(glob, add); },
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    };
    const std::size_t count = rows.size();
    row("pattern:select", i18n::trn("Select {} Item", "Select {} Items", count), false);
    // Adding differs from selecting only when something else is selected.
    const auto selected_in = std::ranges::count_if(
        rows, [&](std::size_t i) { return folder->is_selected(folder->entries()[i].key()); });
    if (folder->selected_count() > static_cast<std::size_t>(selected_in)) {
        row("pattern:add", i18n::trf("Add {} to the Selection", count), true);
    }
    palette_.set_results(std::move(items), i18n::trf("{} of {} match", count, shown));
}

void Browser::select_pattern(const std::string& glob, bool add) {
    if (const std::shared_ptr<Folder> folder = current_folder()) {
        // Matched again: the folder may have changed since the last key.
        const std::vector<std::size_t> rows = folder->matching(glob);
        if (rows.empty()) {
            show_message(std::string{i18n::tr("Nothing matches any more")}, false);
            return;
        }
        folder->select_rows(rows, add);
    }
}

void Browser::refresh_found() {
    if (!finder_ || !palette_.external()) {
        return;
    }
    const std::filesystem::path& root = finder_->root();
    std::vector<PaletteItem> items;
    items.reserve(finder_->results().size());
    for (const FoundFile& found : finder_->results()) {
        SearchResults::Snapshot saved{.root = root,
                                      .query = finder_->query(),
                                      .hidden = finder_->hidden(),
                                      .selected = {found.path},
                                      .cursor = found.path,
                                      .anchor = found.path};
        const auto then = [this, saved, path = found.path](FoundAction after) {
            return [this, saved, path, after]() mutable {
                // The palette closes (and destroys its finder) before running this callback.
                // Its last scroll offset survives close; retain the definition and selected
                // path here so Back can rescan it in the full results view.
                saved.first = palette_.first_row();
                note_location();
                if (!pane().history.empty())
                    pane().history.erase(
                        pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here + 1),
                        pane().history.end());
                pane().history.push_back({.folder = saved.root, .cursor = {}, .search = saved});
                if (pane().history.size() > kHistorySize)
                    pane().history.erase(pane().history.begin());
                pane().here = pane().history.size() - 1;
                show_found(saved.root, path, after);
            };
        };
        items.push_back(
            {.id = "file:" + found.path,
             .label = found.line ? std::format("{}:{} · {}", found.path, found.line, found.context)
                                 : found.path,
             .keys = found.directory ? std::string{i18n::tr("Folder")} : std::string{},
             .remember = false,
             .action = then(FoundAction::show),
             .path = true,
             .quick_look = then(FoundAction::quick_look),
             .open = then(FoundAction::open)});
    }
    palette_.set_results(std::move(items), finder_->status() + " · " +
                                               std::string{i18n::tr("Ctrl+Enter: manage results")});
    if (!finder_->results().empty() && !finder_->scanning() && !finder_->matching())
        start_motion(core::MotionEvent::search_results);
}

void Browser::show_found(const std::filesystem::path& root, const std::string& relative,
                         FoundAction after) {
    const std::filesystem::path target = root / relative;
    const std::filesystem::path parent = target.parent_path();
    quick_look_.close();
    if (pane().mode == Mode::columns) {
        // From the columns' first folder when the file is inside it: the way there stays.
        std::filesystem::path from = root;
        if (pane().columns.column_count() > 0) {
            const std::filesystem::path first = pane().columns.column(0).folder().path();
            const auto [in_first, in_target] = std::ranges::mismatch(first, target);
            if (in_first == first.end()) {
                from = first;
            }
        }
        pane().columns.reveal(from, target);
        redraw_();
    } else {
        open(parent);
        if (const std::shared_ptr<Folder> shown = current_folder()) {
            shown->select_name(target.filename().string());
        }
    }
    note_location();
    found_after_.reset();
    if (after != FoundAction::show) {
        found_after_ =
            FoundAfter{.folder = parent, .name = target.filename().string(), .action = after};
    }
}

void Browser::settle_found(Clock::time_point now) {
    settle_search_preview();
    if (!found_after_) {
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->path() != found_after_->folder || folder->cursor_pending()) {
        return;  // still on its way
    }
    const FoundAfter after = *found_after_;
    found_after_.reset();
    const fs::Entry* const cursor = folder->cursor_entry();
    if (cursor == nullptr || cursor->name != after.name) {
        return;  // gone meanwhile
    }
    if (after.action == FoundAction::quick_look) {
        quick_look_.open(folder, now);
    } else if (!open_selection() && cursor->is_directory()) {
        ui::KeyEvent enter;  // a folder: into it
        enter.keysym = XKB_KEY_Return;
        view_key(enter, now);
    }
    redraw_();
}

}  // namespace rummage::views
