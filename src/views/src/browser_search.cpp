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
    if (root.empty()) {
        const auto folder = current_folder();
        if (!folder || folder->is_trash() || folder->is_archive())
            return;
        root = folder->path();
    }
    palette_.close();
    menu_.close();
    shortcuts_.close();
    quick_look_.close();
    search_preview_.reset();
    search_preview_pending_ = false;
    ++search_generation_;
    if (!search_results_)
        search_results_ = std::make_unique<SearchResults>(
            pool_, redraw_, [this](auto action) { search_action(action); });
    search_results_->open(root, std::move(query), hidden.value_or(pane().show_hidden));
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
    if (!searching_results())
        return;
    const auto paths = search_results_->chosen();
    auto* finder = search_results_->finder();
    if (!finder)
        return;
    switch (action) {
        case Action::close:
            ++search_generation_;
            quick_look_.close();
            search_preview_.reset();
            search_preview_pending_ = false;
            search_results_->close();
            return;
        case Action::saved:
            open_saved_searches();
            return;
        case Action::details:
            copy_text(finder->root().string() + "\n" + finder->query() + "\n" + finder->status());
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
            const State::SavedSearch definition{"", finder->root(), finder->query(),
                                                finder->hidden()};
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
            search_preview_ = std::make_shared<Folder>(loop_, pool_, watcher_, mime_,
                                                       paths.front().parent_path(), redraw_, true);
            search_preview_->select_name(paths.front().filename().string());
            search_preview_pending_ = true;
            redraw_();
            break;
        case Action::reveal:
        case Action::open: {
            if (paths.size() != 1)
                return;
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
    if (searching_results() && search_preview_->cursor_entry())
        quick_look_.open(search_preview_, Clock::now());
    else
        show_message(std::string{i18n::tr("The result is no longer available; refresh the search")},
                     true);
}
}  // namespace rummage::views
