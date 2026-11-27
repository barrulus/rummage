#include <cstdlib>

#include "rummage/core/i18n.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {
namespace {
std::filesystem::path photo_home() {
    if (const char* home = std::getenv("HOME"); home && *home == '/') {
        std::error_code error;
        if (std::filesystem::is_directory(home, error))
            return home;
    }
    return "/";
}
}  // namespace

bool Browser::photos_available() const {
    return photos_enabled_ && !picker_mode_ && !photos::Client::installed_path().empty();
}

void Browser::set_photos_enabled(bool enabled) {
    photos_enabled_ = enabled;
    if (!enabled && photo_library_) {
        quick_look_.close();
        properties_.close();
        photo_state_ = photo_library_->state();
        const auto context = context_area_;
        for (std::size_t index = 0; index < areas_.size(); ++index) {
            if (!areas_[index])
                continue;
            context_area_ = index;
            const auto active = area().active;
            for (std::size_t tab = 0; tab < area().tabs.size(); ++tab) {
                if (!area().tabs[tab]->photos)
                    continue;
                area().active = tab;
                pane().photos = false;
                open(photo_home());
            }
            area().active = active;
        }
        context_area_ = context;
        photo_library_.reset();
        photo_navigation_ = nlohmann::json::object();
        info_pane_.set_photo_properties(std::nullopt);
    }
    show_places();
    redraw_();
}

void Browser::open_photo_library(std::filesystem::path add_folder) {
    if (!photos_available())
        return;
    palette_.close();
    menu_.close();
    shortcuts_.close();
    quick_look_.close();
    if (searching_results())
        search_results_->close();
    prepare_photo_library();
    bool found = false;
    for (std::size_t index = 0; index < areas_.size() && !found; ++index) {
        if (!areas_[index])
            continue;
        for (std::size_t tab = 0; tab < areas_[index]->tabs.size(); ++tab) {
            if (!areas_[index]->tabs[tab]->photos)
                continue;
            leave_pane();
            context_area_ = active_area_ = index;
            area().active = tab;
            found = true;
            break;
        }
    }
    if (!found) {
        note_location();
        leave_pane();
        const auto at = area().active + 1;
        add_pane(at);
        area().active = at;
        pane().photos = true;
        pane().settings_folder.clear();
        pane().mode = Mode::columns;
        pane().columns.show(photo_library_->folder());
        pane().details.set_columns(
            {fs::SortKey::name, fs::SortKey::size, fs::SortKey::modified, fs::SortKey::location});
        set_mode(Mode::grid);
        pane().photo_view = Mode::grid;
    }
    photo_library_->open();
    if (found && add_folder.empty())
        photo_library_->show_all_photos();
    pane().filter_field.set_text(photo_library_->query_text());
    pane().valid_filter = photo_library_->query_text();
    sync_photo_view();
    enter_pane();
    if (!add_folder.empty())
        photo_library_->add_folder(std::move(add_folder));
}

void Browser::prepare_photo_library() {
    if (!photo_library_) {
        photo_library_ = std::make_unique<PhotoLibrary>(loop_, pool_, watcher_, mime_, redraw_);
        photo_library_->set_changed_handler([this] { sync_photo_view(); });
        photo_library_->set_navigation_handler([this](nlohmann::json navigation) {
            photo_navigation_ = std::move(navigation);
            show_places();
        });
        photo_library_->set_folder_chooser([this](const auto& start, auto chosen) {
            if (folder_chooser_)
                folder_chooser_(start.empty() ? photo_home() : start, std::move(chosen));
            else
                show_message(std::string{i18n::tr("The folder chooser is unavailable")}, true);
        });
        photo_library_->set_edit_handler([this] { open_photo_properties(); });
        photo_library_->set_album_chooser([this](std::vector<MenuItem> choices, std::string title) {
            if (!showing_photos() || has_overlay())
                return;
            std::vector<PaletteItem> items;
            for (auto& choice : choices) {
                if (!choice.enabled || !choice.action)
                    continue;
                items.push_back({.id = std::move(choice.id),
                                 .label = std::move(choice.label),
                                 .keys = {},
                                 .remember = false,
                                 .action = std::move(choice.action),
                                 .path = false,
                                 .quick_look = {},
                                 .open = {}});
            }
            palette_use_ = PaletteUse::commands;
            palette_.open(std::move(items), {}, std::move(title));
        });
        photo_library_->set_path_chooser([this](const auto& request, auto chosen) {
            if (photo_path_chooser_)
                photo_path_chooser_(request, std::move(chosen));
            else {
                show_message(std::string{i18n::tr("The file chooser is unavailable")}, true);
                chosen(std::nullopt);
            }
        });
        photo_library_->set_reveal_handler([this](const auto& path) {
            open_tab(path.parent_path(), true);
            if (const auto folder = current_folder())
                folder->select_name(path.filename().string());
        });
        photo_library_->set_file_action([this](auto request) { photo_operation(request); });
    }
    if (!photo_state_.empty()) {
        photo_library_->restore_state(photo_state_);
        photo_state_.clear();
    }
}

void Browser::sync_photo_view() {
    if (!photo_library_)
        return;
    const auto context = context_area_;
    for (std::size_t index = 0; index < areas_.size(); ++index) {
        if (!areas_[index])
            continue;
        context_area_ = index;
        const auto active = area().active;
        for (std::size_t tab = 0; tab < area().tabs.size(); ++tab) {
            if (!area().tabs[tab]->photos)
                continue;
            area().active = tab;
            const auto& query = photo_library_->query_text();
            if (pane().filter_field.text() != query)
                pane().filter_field.set_text(query);
            pane().valid_filter = query;
            const bool records = !photo_library_->photo_rows();
            if (records != pane().photo_records) {
                if (records) {
                    pane().photo_view = pane().mode;
                    pane().photo_columns = pane().details.columns();
                }
                pane().photo_records = records;
                pane().details.set_columns(records ? std::vector<fs::SortKey>{fs::SortKey::name}
                                                   : pane().photo_columns);
                set_mode(records ? Mode::details : pane().photo_view);
            }
        }
        area().active = active;
    }
    context_area_ = context;
    redraw_();
}

void Browser::open_photo_menu(float x, float y, bool keyboard) {
    if (showing_photos())
        menu_.open(x, y, photo_library_->menu_items(), keyboard);
}

void Browser::open_photo_properties() {
    if (!showing_photos() || !photo_library_->photo_rows())
        return;
    auto snapshot = photo_library_->properties();
    if (snapshot.ids.empty())
        return;
    properties_.open_photo(
        std::move(snapshot), [this, alive = alive_](auto ids, auto edits, auto done) {
            if (!*alive || !photo_library_) {
                done(std::string{i18n::tr("Photo Library is no longer available")});
                return;
            }
            photo_library_->save_annotations(std::move(ids), std::move(edits), std::move(done));
        });
}

void Browser::resume_photo_work() {
    if (photo_state_.empty() || !photos_available() || photo_library_)
        return;
    pool_.submit(
        core::Priority::prefetch,
        [](const core::CancelToken&) {
            std::error_code error;
            return std::filesystem::is_regular_file(photos::Client::default_catalog_path(), error);
        },
        [this, alive = alive_](bool exists) {
            if (!*alive || !exists || !photos_available() || photo_library_)
                return;
            prepare_photo_library();
            photo_library_->resume();
        });
}
}  // namespace rummage::views
