#include <algorithm>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::move_into_new_folder() {
    const auto folder = current_folder();
    if (!folder || picker_mode_ || folder->is_trash() || folder->is_archive())
        return;
    std::vector<std::filesystem::path> paths;
    for (const auto& name : targets(*folder))
        paths.push_back(folder->path() / name);
    group_paths(folder->path(), std::move(paths));
}

void Browser::group_paths(const std::filesystem::path& destination,
                          std::vector<std::filesystem::path> paths) {
    if (picker_mode_ || paths.empty() || fs::is_trash(destination))
        return;
    const auto source = paths.front().parent_path();
    if (std::ranges::any_of(paths, [&](const auto& path) { return path.parent_path() != source; }))
        return;
    std::vector<std::string> names;
    for (const auto& path : paths)
        names.push_back(path.filename().string());
    confirm_.open_name(
        std::string{i18n::tr("Move into New Folder…")}, destination.string(),
        std::string{i18n::tr("New Folder")},
        [this, source, destination, names](const std::string& name) {
            if (auto error = fs::name_problem(name))
                return *error;
            submit({.kind = fs::TransferKind::group,
                    .source = source,
                    .names = names,
                    .destination = destination / name},
                   open_batch(Purpose::user, FileOperation::Kind::group));
            status_bar_.set_transfer(TransferStatus{
                .text = std::string{i18n::tr("Getting ready to move…")}, .fraction = -1.0F});
            return std::string{};
        },
        std::string{i18n::tr("Move")});
}

void Browser::extract_dropped_archive(const std::filesystem::path& archive,
                                      const std::filesystem::path& destination) {
    if (picker_mode_ || fs::is_trash(destination))
        return;
    auto name = archive.filename().string();
    if (name.ends_with(".tar.gz"))
        name.resize(name.size() - 7);
    else
        name = archive.stem().string();
    confirm_.open_name(
        std::string{i18n::tr("Extract into New Folder…")}, destination.string(), name,
        [this, archive, destination](const std::string& folder) {
            if (auto error = fs::name_problem(folder))
                return *error;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [archive](const core::CancelToken&) { return fs::archive_tree(archive, nullptr); },
                [this, alive = alive_, archive, destination,
                 folder](const std::shared_ptr<const fs::ArchiveTree>& tree) {
                    if (!*alive)
                        return;
                    --operations_;
                    const auto* entries = tree->folder("");
                    if (tree->failure() != rummage::archive::Failure::none || !entries ||
                        entries->empty()) {
                        show_message(tree->detail().empty()
                                         ? std::string{i18n::tr("No readable archive contents")}
                                         : tree->detail(),
                                     true);
                        return;
                    }
                    std::vector<std::string> names;
                    for (const auto& entry : *entries)
                        names.push_back(entry.name);
                    submit({.kind = fs::TransferKind::copy,
                            .source = archive,
                            .names = std::move(names),
                            .destination = destination,
                            .extract_into = folder,
                            .from_archive = true},
                           open_batch(Purpose::user, FileOperation::Kind::copy));
                    status_bar_.set_transfer(
                        TransferStatus{.text = std::string{i18n::tr("Getting ready to extract…")},
                                       .fraction = -1.0F});
                });
            return std::string{};
        },
        std::string{i18n::tr("Extract")});
}

void Browser::drop_actions(const std::filesystem::path& folder,
                           std::vector<std::filesystem::path> paths, bool own, bool move_allowed) {
    if (paths.empty())
        return;
    std::vector<MenuItem> items;
    const auto add = [&](std::string id, std::string_view label, std::function<void()> action,
                         bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = std::move(action);
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    MenuItem destination;
    destination.id = "drop_destination";
    destination.label = i18n::trf("Destination: {}", folder.string());
    destination.enabled = false;
    items.push_back(std::move(destination));
    const bool normal = refusal(folder, paths).empty();
    add(
        "drop_copy", i18n::mark("Copy Here"),
        [this, folder, paths] { transfer_paths(folder, paths, false, std::nullopt); }, normal);
    if (own || move_allowed) {
        add(
            "drop_move", i18n::mark("Move Here"),
            [this, folder, paths] { transfer_paths(folder, paths, true, std::nullopt); }, normal);
    }
    add("drop_link", i18n::mark("Link Here"),
        [this, folder, paths] { link_paths(folder, paths, fs::LinkKind::absolute); });
    if (own) {
        add("move_into_folder", i18n::mark("Move into New Folder…"),
            [this, folder, paths] { group_paths(folder, paths); });
    }
    fs::Entry entry;
    entry.name = paths.front().filename().string();
    if (paths.size() == 1 && opens_as_folder(entry))
        add("drop_extract", i18n::mark("Extract into New Folder…"),
            [this, archive = paths.front(), folder] { extract_dropped_archive(archive, folder); });
    add("drop_cancel", i18n::mark("Cancel"), [] {});
    menu_.open(drop_x_, drop_y_, std::move(items), true);
}

std::vector<MenuItem> Browser::sidebar_menu() {
    std::vector<MenuItem> items;
    const auto add = [&](std::string id, std::string_view label, bool checked,
                         std::function<void()> action) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.checkable = true;
        item.checked = checked;
        item.action = std::move(action);
        items.push_back(std::move(item));
    };
    add("sidebar_visible", i18n::mark("Show Sidebar"), sidebar_visible_,
        [this] { set_sidebar_visible(!sidebar_visible_); });
    for (const auto& [section, label] :
         {std::pair{fs::Place::Section::places, i18n::mark("Places")},
          std::pair{fs::Place::Section::bookmarks, i18n::mark("Bookmarks")},
          std::pair{fs::Place::Section::devices, i18n::mark("Devices")}}) {
        const auto key = std::to_string(static_cast<int>(section));
        add("sidebar_section_" + key, label,
            std::ranges::find(sidebar_.collapsed(), key) == sidebar_.collapsed().end(),
            [this, section] { sidebar_.toggle_section(section); });
        if (section == fs::Place::Section::places)
            items.back().separator = true;
    }
    MenuItem restore;
    restore.id = "sidebar_restore";
    restore.label = i18n::tr("Restore Hidden Places");
    restore.separator = true;
    restore.action = [this] { sidebar_.restore_places(); };
    items.push_back(std::move(restore));
    MenuItem reset;
    reset.id = "sidebar_reset";
    reset.label = i18n::tr("Reset Sidebar Organization");
    reset.action = [this] { sidebar_.set_organization({}, {}, {}); };
    items.push_back(std::move(reset));
    return items;
}

std::vector<MenuItem> Browser::sidebar_organization_items(const fs::Place& place) {
    std::vector<MenuItem> items;
    if (Sidebar::child(place))
        return items;
    const auto add = [&](std::string id, std::string_view label, std::function<void()> action) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = std::move(action);
        item.separator = items.empty();
        items.push_back(std::move(item));
    };
    const bool bookmark = place.section == fs::Place::Section::bookmarks && place.uri.empty();
    add(bookmark ? "move_bookmark_up" : "sidebar_move_up", i18n::mark("Move Up"),
        [this, place] { sidebar_.move_place(place, -1); });
    add(bookmark ? "move_bookmark_down" : "sidebar_move_down", i18n::mark("Move Down"),
        [this, place] { sidebar_.move_place(place, 1); });
    std::vector<fs::Place> shown;
    for (const auto& root : sidebar_.places())
        if (!Sidebar::child(root))
            shown.push_back(root);
    const auto at = std::ranges::find_if(
        shown, [&](const auto& p) { return Sidebar::place_key(p) == Sidebar::place_key(place); });
    items[0].enabled =
        at != shown.end() && at != shown.begin() && std::prev(at)->section == place.section;
    items[1].enabled = at != shown.end() && std::next(at) != shown.end() &&
                       std::next(at)->section == place.section;
    if (Sidebar::separator(place)) {
        add("sidebar_remove_separator", i18n::mark("Remove Separator"),
            [this, place] { sidebar_.remove_separator(place); });
    } else {
        add("sidebar_separator", i18n::mark("Add Separator Above"),
            [this, place] { sidebar_.add_separator(place); });
        add("sidebar_hide", i18n::mark("Hide from Sidebar"),
            [this, place] { sidebar_.hide_place(place); });
    }
    MenuItem options;
    options.id = "sidebar_options";
    options.label = i18n::tr("Sidebar");
    options.submenu = true;
    options.children = sidebar_menu();
    items.push_back(std::move(options));
    return items;
}

std::vector<MenuItem> Browser::place_navigation_items(const fs::Place& place) {
    if (Sidebar::separator(place))
        return sidebar_organization_items(place);
    std::vector<MenuItem> items;
    if (place.uri == "rummage:saved-searches") {
        MenuItem item;
        item.id = "saved_searches";
        item.label = i18n::tr("Saved Searches…");
        item.action = [this] { open_saved_searches(); };
        items.push_back(std::move(item));
        return items;
    }
    const bool mounted = place.disk.mounted && !place.path.empty();
    const bool trash = fs::is_trash(place.path);
    const auto add = [&](std::string id, std::string_view label, std::function<void()> action,
                         bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = std::move(action);
        item.enabled = enabled;
        if (item.id == "open")
            item.keys = keys_for(Command::open_selected);
        else if (item.id == "open_tab")
            item.keys = keys_for(Command::open_selected_tab);
        else if (item.id == "open_other_pane")
            item.keys = keys_for(Command::open_other_pane);
        else if (item.id == "open_terminal")
            item.keys = keys_for(Command::open_terminal);
        else if (item.id == "properties")
            item.keys = keys_for(Command::properties);
        else if (item.id == "copy_path")
            item.keys = keys_for(Command::copy_path);
        items.push_back(std::move(item));
    };
    add(
        "open", i18n::mark("Open"),
        [this, place] {
            sidebar_.set_focused(false);
            open_place(place);
        },
        mounted || (!place.uri.empty() ? bool(remotes_.mount) : bool(devices_.mount)));
    if (panes_enabled_ && !picker_mode_) {
        add(
            "open_tab", i18n::mark("Open in New Tab"),
            [this, place] {
                sidebar_.set_focused(false);
                open_place(place, PlaceDestination::tab);
            },
            mounted || (!place.uri.empty() ? bool(remotes_.mount) : bool(devices_.mount)));
        if (new_window_)
            add(
                "open_window", i18n::mark("Open in New Window"),
                [this, place] { open_place(place, PlaceDestination::window); },
                mounted || (!place.uri.empty() ? bool(remotes_.mount) : bool(devices_.mount)));
        if (!trash)
            add(
                "open_other_pane", i18n::mark("Open in Other Pane"),
                [this, path = place.path] {
                    sidebar_.set_focused(false);
                    open_in_other_pane(path);
                },
                mounted);
    }
    if (!trash) {
        if (terminal_)
            add(
                "open_terminal", i18n::mark("Open in Terminal"),
                [this, path = place.path] { terminal_(path); }, mounted);
        add(
            "copy_path", i18n::mark("Copy Path"),
            [this, place] { copy_text(place.uri.empty() ? place.path.string() : place.uri); },
            mounted || !place.uri.empty());
        if (place.section != fs::Place::Section::devices || Sidebar::child(place))
            add(
                "properties", i18n::mark("Properties…"),
                [this, path = place.path] { open_properties(path); }, mounted);
    }
    auto organization = sidebar_organization_items(place);
    items.insert(items.end(), organization.begin(), organization.end());
    return items;
}
}  // namespace rummage::views
