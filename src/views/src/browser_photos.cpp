#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rummage/core/i18n.hpp"
#include "rummage/photos/client.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/picture/png.hpp"
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

// File URIs remain usable immediately. This optional payload is bounded, built on a worker,
// and decoded only through the same isolated engine used by the rest of the browser.
std::vector<SystemClipboard::Format> photo_image_formats(const std::filesystem::path& path,
                                                         const fs::MimeDatabase& mime,
                                                         const core::CancelToken& cancel) {
    try {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
        if (fd < 0)
            return {};

        struct Close {
            int fd;

            ~Close() { ::close(fd); }
        } close{fd};
        struct stat before{};
        if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
            static_cast<std::uint64_t>(before.st_size) > kMaxContentBytes)
            return {};
        std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
        std::size_t read = 0;
        while (read < bytes.size()) {
            if (cancel.cancelled())
                return {};
            const auto count = ::read(fd, bytes.data() + read,
                                      std::min(bytes.size() - read, std::size_t{256U << 10U}));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return {};
            read += static_cast<std::size_t>(count);
        }
        const auto unchanged = [&] {
            struct stat current{};
            return ::lstat(path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
                   current.st_dev == before.st_dev && current.st_ino == before.st_ino &&
                   current.st_size == before.st_size &&
                   current.st_mtim.tv_sec == before.st_mtim.tv_sec &&
                   current.st_mtim.tv_nsec == before.st_mtim.tv_nsec &&
                   current.st_ctim.tv_sec == before.st_ctim.tv_sec &&
                   current.st_ctim.tv_nsec == before.st_ctim.tv_nsec;
        };
        if (cancel.cancelled() || !unchanged())
            return {};
        const std::string type{mime.name(
            mime.detect(path.filename().string(),
                        std::as_bytes(std::span{bytes.data(), bytes.size()}), bytes.size()))};
        std::vector<SystemClipboard::Format> result;
        if (type.starts_with("image/") && content_extension(type, bytes))
            result.emplace_back(type, std::move(bytes));
        if (type == "image/png")
            return result;  // Preserve PNG bytes, colour metadata and animation exactly.
        if (!type.starts_with("image/"))
            return result;
        const auto loaded = picture::load_picture(
            {.path = path, .max_width = 4096, .max_height = 4096, .mime = type, .cancel = &cancel});
        // Never silently substitute a reduced preview for a copied original. Larger images
        // retain their original image MIME and file offer even when no PNG can be supplied.
        if (!cancel.cancelled() && !loaded.image.empty() && !loaded.from_preview &&
            loaded.full_width > 0 && loaded.full_height > 0 &&
            loaded.image.width == loaded.full_width && loaded.image.height == loaded.full_height &&
            loaded.image.width <= 4096 && loaded.image.height <= 4096) {
            const auto png = picture::encode_png(loaded.image);
            if (!cancel.cancelled() && !png.empty() && png.size() <= kMaxContentBytes)
                result.insert(result.begin(),
                              {"image/png",
                               std::string{reinterpret_cast<const char*>(png.data()), png.size()}});
        }
        return !cancel.cancelled() && unchanged() ? result : std::vector<SystemClipboard::Format>{};
    } catch (const std::exception&) {
        return {};  // Image enrichment cannot invalidate an already usable file clipboard.
    }
}
}  // namespace

void Browser::copy_photo_files(bool cut) {
    photo_copy_job_.cancel();
    const auto epoch = ++photo_copy_epoch_;
    if (!showing_photos() || !photo_library_->photo_rows())
        return;
    const auto rows = photo_library_->selected_photos();
    if (rows.size() != photo_library_->selected_ids().size()) {
        show_message(std::string{i18n::tr(
                         "Load the selected photos before copying or moving this selection")},
                     true);
        return;
    }
    if (rows.empty())
        return;
    std::vector<std::filesystem::path> paths;
    for (const auto& row : rows) {
        const auto availability = row.value("availability", "");
        if (availability != "online" && (cut || availability != "changed")) {
            show_message(std::string{i18n::tr(
                             "Some selected originals are unavailable or need a source refresh")},
                         true);
            return;
        }
        paths.emplace_back(row.at("path").get<std::string>());
    }
    clipboard_ = {.directory = {}, .names = {}, .cut = cut, .paths = paths};
    cut_marks_ = {};
    const auto text = path_lines(paths);
    std::vector<SystemClipboard::Format> formats{
        {std::string{kGnomeFiles}, gnome_copied_files(cut, paths)},
        {std::string{kUriList}, uri_list(paths)},
        {std::string{kText}, text},
        {"UTF8_STRING", text},
        {"text/plain", text}};
    if (cut)
        formats.emplace_back(std::string{kKdeCut}, "1");
    if (system_.offer)
        system_.offer(formats);
    const bool prepare_image = !cut && paths.size() == 1 && system_.offer;
    show_message(
        std::string{cut             ? i18n::tr("Paste in a folder to move the selected originals")
                    : prepare_image ? i18n::tr("Photo file copied; preparing image clipboard…")
                                    : i18n::tr("Photos copied")},
        false);
    if (cut || paths.size() != 1 || !system_.offer)
        return;
    const auto generation = system_.generation ? std::optional{system_.generation()} : std::nullopt;
    photo_copy_job_ = pool_.submit(
        core::Priority::visible,
        [path = paths.front(), mime = mime_](const core::CancelToken& cancel) {
            return photo_image_formats(path, *mime, cancel);
        },
        [this, alive = alive_, epoch, generation, paths,
         formats = std::move(formats)](std::vector<SystemClipboard::Format> images) mutable {
            if (!*alive || epoch != photo_copy_epoch_ || !system_.offer || clipboard_.cut ||
                clipboard_.paths != paths || !clipboard_.directory.empty() ||
                (system_.owned && !system_.owned()) ||
                (generation && (!system_.generation || system_.generation() != *generation)))
                return;
            if (images.empty()) {
                show_message(
                    std::string{i18n::tr("Photo file copied; image clipboard data is unavailable")},
                    false);
                return;
            }
            for (auto& image : images)
                formats.push_back(std::move(image));
            system_.offer(std::move(formats));
            show_message(std::string{i18n::tr("Photo copied")}, false);
        });
}

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
