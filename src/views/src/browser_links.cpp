#include <algorithm>
#include <cerrno>
#include <map>
#include <sys/stat.h>
#include <system_error>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
void Browser::links_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    const std::size_t made = result.placed.size();
    const std::string what = i18n::trn("{} link", "{} links", made);
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(i18n::trf("Stopped: {} made so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(i18n::trf("Made {}; {} could not be: {}", what, result.errors.size(),
                               result.errors.front()),
                     true);
    } else if (result.placed.empty()) {
        show_message(std::string{i18n::tr("Nothing to do: every name was skipped")}, false);
    } else {
        show_message(i18n::trf("Made {}", what), false);
    }
    // Selected, ready to rename or drag away, when their folder is the one shown.
    if (const std::shared_ptr<Folder> folder = current_folder();
        folder && folder->path() == request.destination && !result.placed.empty()) {
        folder->expect_names(result.placed);
    }
}

void Browser::create_links(fs::LinkKind kind) {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || picker_mode_ || folder->is_trash() || folder->is_archive() ||
        folder->is_collection()) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& name : names)
        paths.push_back(folder->path() / name);
    link_paths(folder->path(), paths, kind);
}

void Browser::link_paths(const std::filesystem::path& destination,
                         const std::vector<std::filesystem::path>& paths, fs::LinkKind kind) {
    if (picker_mode_ || paths.empty() || fs::is_trash(destination))
        return;
    std::map<std::filesystem::path, std::vector<std::string>> groups;
    for (const auto& path : paths) {
        if (path.is_absolute() && !fs::is_trash(path.parent_path()))
            groups[path.parent_path()].push_back(path.filename().string());
    }
    if (groups.empty())
        return;
    // Recorded as a copy, as Compress is: Ctrl+Z moves the new links to the trash, never what
    // they lead to.
    const auto batch = open_batch(Purpose::user, FileOperation::Kind::copy);
    for (auto& [source, names] : groups)
        submit({.kind = fs::TransferKind::link,
                .source = source,
                .names = std::move(names),
                .destination = destination,
                .link_kind = kind},
               batch);
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(TransferStatus{
            .text = i18n::trf("Making {}…", i18n::trn("{} link", "{} links", paths.size())),
            .fraction = -1.0F});
    }
    redraw_();
}

void Browser::paste_links(fs::LinkKind kind) {
    const auto folder = current_folder();
    if (!folder || picker_mode_ || folder->is_archive() || folder->is_trash() ||
        folder->is_collection())
        return;
    if (system_.types && system_.read &&
        (!system_.owned || !system_.owned() || clipboard_.names.empty())) {
        const auto types = system_.types();
        const bool gnome = std::ranges::find(types, kGnomeFiles) != types.end();
        if (!gnome && std::ranges::find(types, kUriList) == types.end()) {
            show_message(std::string{i18n::tr("The clipboard holds no files")}, false);
            return;
        }
        ++operations_;
        system_.read(std::string{gnome ? kGnomeFiles : kUriList},
                     [this, alive = alive_, destination = folder->path(), kind,
                      gnome](const std::optional<std::string>& data) {
                         if (!*alive)
                             return;
                         --operations_;
                         const auto files = data && data->size() <= kMaxContentBytes
                                                ? (gnome ? parse_gnome_copied_files(*data)
                                                         : std::optional{parse_uri_list(*data)})
                                                : std::nullopt;
                         if (!files || files->paths.empty())
                             show_message(std::string{i18n::tr("The clipboard holds no files")},
                                          true);
                         else
                             link_paths(destination, files->paths, kind);
                     });
        return;
    }
    if (!clipboard_.paths.empty()) {
        link_paths(folder->path(), clipboard_.paths, kind);
        return;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& name : clipboard_.names)
        paths.push_back(clipboard_.directory / name);
    link_paths(folder->path(), paths, kind);
}

std::vector<MenuItem> Browser::link_menu(bool paste) {
    std::vector<MenuItem> items;
    const auto add = [&](std::string id, std::string_view label, fs::LinkKind kind) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.action = [this, paste, kind] {
            if (paste)
                paste_links(kind);
            else
                create_links(kind);
        };
        items.push_back(std::move(item));
    };
    add(paste ? "paste_absolute_link" : "create_link",
        paste ? i18n::mark("Paste Absolute Links") : i18n::mark("Create Absolute Link"),
        fs::LinkKind::absolute);
    add(paste ? "paste_relative_link" : "create_relative_link",
        paste ? i18n::mark("Paste Relative Links") : i18n::mark("Create Relative Link"),
        fs::LinkKind::relative);
    add(paste ? "paste_hard_link" : "create_hard_link",
        paste ? i18n::mark("Paste Hard Links") : i18n::mark("Create Hard Link"),
        fs::LinkKind::hard);
    return items;
}

void Browser::reveal_link_target() {
    const auto folder = current_folder();
    if (!folder || folder->is_trash() || folder->is_archive() || folder->is_collection())
        return;
    const auto names = targets(*folder);
    if (names.size() != 1)
        return;
    const auto generation = ++path_navigation_generation_;
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [path = folder->path() / names.front()](const core::CancelToken&) {
            std::error_code error;
            auto target = std::filesystem::read_symlink(path, error);
            if (!error) {
                if (target.is_relative())
                    target = path.parent_path() / target;
                // Resolve intermediate directory links (including before ".."),
                // but preserve a chained link in the final component.
                const auto name = target.filename();
                if (name.empty() || name == "." || name == "..")
                    target = std::filesystem::canonical(target, error);
                else {
                    const auto parent = std::filesystem::canonical(target.parent_path(), error);
                    if (!error)
                        target = parent / name;
                }
                struct stat info{};
                if (!error && lstat(target.c_str(), &info) != 0)
                    error = {errno, std::generic_category()};
            }
            return std::pair{target, error};
        },
        [this, alive = alive_, folder, generation](const auto& result) {
            if (!*alive)
                return;
            --operations_;
            if (generation != path_navigation_generation_ || current_folder() != folder)
                return;
            const auto& [target, error] = result;
            if (error) {
                show_message(i18n::trf("Cannot reveal link target: {}", error.message()), true);
            } else if (target == target.root_path()) {
                open(target);
            } else {
                reveal(target.parent_path(), target);
            }
        });
}

}  // namespace rummage::views
