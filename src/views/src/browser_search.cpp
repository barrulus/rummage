#include <algorithm>
#include <format>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_clipboard.hpp"

namespace rummage::views {
void Browser::open_search_results(std::filesystem::path root, std::string query,
                                  std::optional<bool> hidden) {
    if (picker_mode_)
        return;
    if (root.empty() && showing_photos()) {
        palette_.close();
        menu_.close();
        if (!query.empty())
            set_name_filter(std::move(query));
        filter_focused_ = true;
        pane().filter_field.select_all();
        redraw_();
        return;
    }
    if (searching_results() && root.empty() && query.empty()) {
        filter_focused_ = true;
        pane().filter_field.select_all();
        redraw_();
        return;
    }
    if (root.empty() && searching_results())
        root = pane().search_results->snapshot().root;
    if (root.empty()) {
        const auto folder = current_folder();
        if (!folder || folder->is_trash() || folder->is_archive() || folder->is_collection())
            return;
        root = folder->path();
    }
    if (!root.is_absolute() && !fs::is_recent(root))
        return;
    // An explicit filesystem search opens in a normal tab, preserving the logical photo tab.
    if (showing_photos())
        open_tab(root, true);
    note_location();
    remember_folder(pane());
    filter_focused_ = true;
    palette_.close();
    menu_.close();
    shortcuts_.close();
    quick_look_.close();
    search_preview_.reset();
    search_preview_pending_ = false;
    ++search_generation_;
    pane().search_folder.reset();
    pane().search_paths.clear();
    pane().restoring_search.reset();
    if (!pane().search_results)
        pane().search_results = std::make_unique<SearchResults>(
            pool_,
            [this, target = &pane()] {
                sync_search_results(*target);
                redraw_();
            },
            [this](auto action) { search_action(action); });
    pane().search_results->open(root, std::move(query), hidden.value_or(pane().show_hidden),
                                root != "rummage:recent");
    show_search_folder();
    if (!pane().history.empty() && pane().history[pane().here].search) {
        pane().history[pane().here].search = search_snapshot();
    } else {
        if (!pane().history.empty())
            pane().history.erase(
                pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here + 1),
                pane().history.end());
        pane().history.push_back({.folder = root, .cursor = {}, .search = search_snapshot()});
        if (pane().history.size() > kHistorySize)
            pane().history.erase(pane().history.begin());
        pane().here = pane().history.size() - 1;
    }
}

void Browser::show_search_folder() {
    const auto saved = pane().search_results->snapshot();
    pane().settings_folder.clear();
    pane().retained_folder_mode.reset();
    if (!pane().filter_folder.empty() && !pane().valid_filter.empty())
        pane().folder_filters[pane().filter_folder] = pane().valid_filter;
    pane().filter_folder = fs::search_path();
    pane().valid_filter.clear();
    pane().filter_error.clear();
    pane().filter_field.set_text(saved.query);
    pane().entry_filter = {};
    pane().columns.set_entry_filter({});
    pane().grid.set_entry_filter({});
    pane().details.set_entry_filter({});
    pane().search_paths.clear();
    pane().search_folder = std::make_shared<Folder>(
        loop_, pool_, watcher_, mime_, fs::search_path(),
        [this, alive = alive_] {
            if (!*alive)
                return;
            // Operations can retain a Folder after its tab closes; never capture its Pane.
            for (const auto& owner : areas_)
                if (owner)
                    for (const auto& tab : owner->tabs)
                        sync_search_results(*tab);
            redraw_();
        },
        true);
    pane().columns.show(pane().search_folder);
    if (pane().mode == Mode::grid) {
        pane().columns.suspend();
        pane().grid.show(pane().search_folder);
    } else if (pane().mode == Mode::details) {
        pane().columns.suspend();
        pane().details.show(pane().search_folder);
    }
    sync_search_results(pane());
}

SearchResults::Snapshot Browser::search_snapshot() const {
    auto saved = pane().search_results->snapshot();
    if (pane().restoring_search)
        return *pane().restoring_search;
    if (const auto& folder = pane().search_folder) {
        const auto selected = folder->selected_names();
        saved.selected = {selected.begin(), selected.end()};
        saved.cursor = folder->cursor_entry() ? folder->cursor_entry()->name : "";
        saved.anchor = saved.cursor;
    }
    return saved;
}

void Browser::sync_search_results(Pane& target) {
    if (!target.search_results || !target.search_results->is_open() || !target.search_folder)
        return;
    const auto saved = target.search_results->snapshot();
    const auto root =
        target.search_results->finder()->recent() ? std::filesystem::path{"/"} : saved.root;
    std::vector<std::filesystem::path> paths;
    for (const auto& row : target.search_results->rows())
        paths.push_back(root / row.path);
    if (paths != target.search_paths) {
        target.search_paths = paths;
        target.search_folder->set_search_results(std::move(paths));
    }
    if (target.restoring_search && !target.search_results->loading() &&
        !target.search_folder->model().loading()) {
        const auto restore = std::move(*target.restoring_search);
        target.restoring_search.reset();
        const auto identity = [&](const std::string& value) {
            return value.empty() ? std::string{} : (root / value).lexically_normal().string();
        };
        target.search_folder->select_name(identity(restore.cursor));
        std::vector<std::size_t> selected;
        const auto& entries = target.search_folder->entries();
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (std::ranges::any_of(restore.selected, [&](const auto& name) {
                    return identity(name) == entries[i].name;
                }))
                selected.push_back(i);
        target.search_folder->select_rows(selected, false);
    }
}

void Browser::open_saved_searches(bool remove) {
    if (picker_mode_)
        return;
    palette_.close();
    menu_.close();
    std::vector<PaletteItem> items;
    const auto add = [&](std::string id, std::string label, std::function<void()> action) {
        items.push_back({.id = std::move(id),
                         .label = std::move(label),
                         .keys = {},
                         .remember = false,
                         .action = std::move(action),
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    };
    for (const auto& search : saved_searches_) {
        if (!remove) {
            add("search:" + search.name,
                std::format("{} · {} · {}", search.name, search.root.string(), search.query),
                [this, search] { open_search_results(search.root, search.query, search.hidden); });
        } else {
            add("search:remove:" + search.name, i18n::trf("Remove saved search: {}", search.name),
                [this, name = search.name] {
                    std::erase_if(saved_searches_,
                                  [&](const auto& saved) { return saved.name == name; });
                    show_places();
                    open_saved_searches(true);
                });
        }
    }
    if (!remove && !saved_searches_.empty())
        add("search:remove", std::string{i18n::tr("Remove Saved Search…")},
            [this] { open_saved_searches(true); });
    palette_.open(std::move(items), {},
                  std::string{saved_searches_.empty() ? i18n::tr("No saved searches")
                                                      : i18n::tr("Saved Searches…")});
}

void Browser::search_action(SearchResults::Action action) {
    using Action = SearchResults::Action;
    const auto collection = current_folder();
    const bool recent = collection && collection->is_recent() && !searching_results();
    if (!searching_results() && !recent)
        return;
    std::vector<std::filesystem::path> paths;
    if (collection && (collection->is_search() || collection->is_recent()))
        for (const auto& name : targets(*collection))
            paths.push_back(collection->model().source_path(name));
    auto* finder = recent ? nullptr : pane().search_results->finder();
    if (!finder && !recent)
        return;
    switch (action) {
        case Action::close: {
            filter_focused_ = false;
            if (can_travel(-1)) {
                travel(-1);
                return;
            }
            const auto root = pane().search_results->snapshot().root;
            note_location();
            ++search_generation_;
            quick_look_.close();
            search_preview_.reset();
            search_preview_pending_ = false;
            pane().search_results->close();
            open(root);
            return;
        }
        case Action::saved:
            open_saved_searches();
            return;
        case Action::details:
            copy_text(pane().search_results->snapshot().root.string() + "\n" + finder->query() +
                      "\n" + finder->status());
            return;
        case Action::undo:
            run(Command::undo, Clock::now());
            return;
        case Action::redo:
            run(Command::redo, Clock::now());
            return;
        case Action::save: {
            if (!finder->specification().error.empty()) {
                show_message(finder->specification().error, true);
                return;
            }
            const auto snapshot = pane().search_results->snapshot();
            const State::SavedSearch definition{"", snapshot.root, snapshot.query, snapshot.hidden};
            confirm_.open_name(
                std::string{i18n::tr("Save Search…")}, definition.root.string(), "",
                [this, definition](const std::string& name) {
                    if (name.empty() || name.size() > 128 ||
                        name.find_first_of("\r\n") != std::string::npos ||
                        name.find('\0') != std::string::npos)
                        return std::string{i18n::tr("Enter a name of at most 128 bytes")};
                    if (std::ranges::find(saved_searches_, name, &State::SavedSearch::name) !=
                        saved_searches_.end())
                        return std::string{i18n::tr("A saved search already has this name")};
                    if (saved_searches_.size() == State::kMaxSavedSearches)
                        return std::string{
                            i18n::tr("Remove a saved search before adding another (limit 64)")};
                    if (definition.query.size() > 16384)
                        return std::string{i18n::tr("The search query is too long to save")};
                    auto saved = definition;
                    saved.name = name;
                    saved_searches_.push_back(std::move(saved));
                    show_places();
                    show_message(std::string{i18n::tr("Search saved")}, false);
                    return std::string{};
                });
            return;
        }
        default:
            break;
    }
    if (paths.empty())
        return;
    switch (action) {
        case Action::copy:
        case Action::cut: {
            const bool cut = action == Action::cut;
            clipboard_ = {.directory = {}, .names = {}, .cut = cut, .paths = paths};
            cut_marks_ = {};
            if (collection && cut) {
                const auto names = targets(*collection);
                cut_marks_ = {.directory = collection->path(),
                              .names = {names.begin(), names.end()}};
            }
            if (system_.offer) {
                std::vector<SystemClipboard::Format> formats{
                    {std::string{kGnomeFiles}, gnome_copied_files(cut, paths)},
                    {std::string{kUriList}, uri_list(paths)},
                    {std::string{kText}, path_lines(paths)},
                    {"UTF8_STRING", path_lines(paths)},
                    {"text/plain", path_lines(paths)}};
                if (cut)
                    formats.emplace_back(std::string{kKdeCut}, "1");
                system_.offer(std::move(formats));
            }
            const auto items = i18n::trn("{} item", "{} items", paths.size());
            show_message(cut ? i18n::trf("Ctrl+V moves {} where you want them", items)
                             : i18n::trf("Ctrl+V copies {} where you want them", items),
                         false);
            break;
        }
        case Action::trash:
            trash_paths(paths, Purpose::user);
            break;
        case Action::rename: {
            if (paths.size() != 1)
                return;
            const auto path = paths.front();
            auto folder = std::make_shared<Folder>(loop_, pool_, watcher_, mime_,
                                                   path.parent_path(), redraw_, true);
            confirm_.open_name(
                std::string{i18n::tr("Rename…")}, path.parent_path().string(),
                path.filename().string(),
                [this, folder, from = path.filename().string()](const std::string& to) {
                    if (to.empty() || to == "." || to == ".." ||
                        to.find('/') != std::string::npos || to.find('\0') != std::string::npos)
                        return std::string{i18n::tr("Invalid name")};
                    if (to != from)
                        rename(folder, from, to);
                    return std::string{};
                });
            break;
        }
        case Action::preview:
            if (paths.size() != 1)
                return;
            {
                std::error_code error;
                if (!std::filesystem::exists(paths.front(), error)) {
                    show_message(std::string{i18n::tr(
                                     "The result is no longer available; refresh the search")},
                                 true);
                    return;
                }
            }
            if (collection && (collection->is_search() || collection->is_recent())) {
                quick_look_.open(collection, Clock::now());
                return;
            }
            if (paths.size() != 1)
                return;
            search_preview_ = std::make_shared<Folder>(loop_, pool_, watcher_, mime_,
                                                       paths.front().parent_path(), redraw_, true);
            search_preview_->select_name(paths.front().filename().string());
            search_preview_pending_ = true;
            redraw_();
            break;
        case Action::reveal:
        case Action::open: {
            if (action == Action::open) {
                open_selection();
                return;
            }
            if (paths.size() != 1)
                return;
            if (collection && (collection->is_search() || collection->is_recent())) {
                go_to(paths.front().parent_path(), paths.front().filename().string());
                return;
            }
            const auto root = finder->root();
            const auto relative = paths.front().lexically_relative(root).string();
            search_action(Action::close);
            show_found(root, relative,
                       action == Action::open ? FoundAction::open : FoundAction::show);
            break;
        }
        default:
            break;
    }
}

void Browser::settle_search_preview() {
    if (!search_preview_pending_ || !search_preview_ || search_preview_->model().loading())
        return;
    search_preview_pending_ = false;
    if ((searching_results() || showing_photos()) && search_preview_->cursor_entry())
        quick_look_.open(search_preview_, Clock::now());
    else
        show_message(std::string{i18n::tr("The result is no longer available; refresh the search")},
                     true);
}
}  // namespace rummage::views
