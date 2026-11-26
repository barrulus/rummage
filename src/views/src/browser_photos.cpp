#include "rummage/core/i18n.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {

bool Browser::photos_available() const {
    return photos_enabled_ && !picker_mode_ && !photos::Client::installed_path().empty();
}

void Browser::set_photos_enabled(bool enabled) {
    photos_enabled_ = enabled;
    if (!enabled && photo_library_) {
        quick_look_.close();
        search_preview_.reset();
        search_preview_pending_ = false;
        photo_state_ = photo_library_->state();
        photo_library_.reset();
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
    search_preview_.reset();
    search_preview_pending_ = false;
    prepare_photo_library();
    photo_library_->open();
    if (!add_folder.empty())
        photo_library_->add_folder(std::move(add_folder));
}

void Browser::prepare_photo_library() {
    if (!photo_library_) {
        photo_library_ = std::make_unique<PhotoLibrary>(
            loop_, pool_, thumbnails_.get(), redraw_,
            [this](PhotoLibrary::Action action, const std::filesystem::path& path) {
                if (action == PhotoLibrary::Action::close) {
                    quick_look_.close();
                    search_preview_.reset();
                    search_preview_pending_ = false;
                    photo_library_->close();
                } else if (action == PhotoLibrary::Action::preview) {
                    search_preview_ = std::make_shared<Folder>(loop_, pool_, watcher_, mime_,
                                                               path.parent_path(), redraw_, true);
                    search_preview_->select_name(path.filename().string());
                    search_preview_pending_ = true;
                    redraw_();
                } else {
                    photo_library_->close();
                    show_found(path.parent_path(), path.filename().string(), FoundAction::show);
                }
            },
            &watcher_);
    }
    if (!photo_state_.empty()) {
        photo_library_->restore_state(photo_state_);
        photo_state_.clear();
    }
    photo_library_->set_file_action([this](auto request) { photo_operation(request); });
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
