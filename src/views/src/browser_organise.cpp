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
                           std::vector<std::filesystem::path> paths, bool own) {
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
    const bool normal = refusal(folder, paths).empty();
    add(
        "drop_copy", i18n::mark("Copy Here"),
        [this, folder, paths] { transfer_paths(folder, paths, false, std::nullopt); }, normal);
    if (own) {
        add(
            "drop_move", i18n::mark("Move Here"),
            [this, folder, paths] { transfer_paths(folder, paths, true, std::nullopt); }, normal);
        add("move_into_folder", i18n::mark("Move into New Folder…"),
            [this, folder, paths] { group_paths(folder, paths); });
    }
    fs::Entry entry;
    entry.name = paths.front().filename().string();
    if (paths.size() == 1 && opens_as_folder(entry))
        add("drop_extract", i18n::mark("Extract into New Folder…"),
            [this, archive = paths.front(), folder] { extract_dropped_archive(archive, folder); });
    menu_.open(drop_x_, drop_y_, std::move(items), false);
}

std::vector<MenuItem> Browser::place_navigation_items(const fs::Place& place) {
    std::vector<MenuItem> items;
    const bool mounted = place.disk.mounted && !place.path.empty();
    const bool trash = fs::is_trash(place.path);
    const auto add = [&](std::string id, std::string_view label, std::function<void()> action,
                         bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = std::move(action);
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    add(
        "open", i18n::mark("Open"), [this, place] { open_place(place); },
        mounted || (!place.uri.empty() ? bool(remotes_.mount) : bool(devices_.mount)));
    if (panes_enabled_ && !picker_mode_) {
        add(
            "open_tab", i18n::mark("Open in New Tab"),
            [this, path = place.path] { open_tab(path, true); }, mounted);
        if (!trash)
            add(
                "open_other_pane", i18n::mark("Open in Other Pane"),
                [this, path = place.path] { open_in_other_pane(path); }, mounted);
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
        if (place.section != fs::Place::Section::devices)
            add(
                "properties", i18n::mark("Properties…"),
                [this, path = place.path] {
                    properties_.open(path, [this] {
                        info_pane_.refresh();
                        set_places(places_);
                    });
                },
                mounted);
    }
    return items;
}
}  // namespace rummage::views
