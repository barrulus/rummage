#include "rummage/views/browser.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/core/memory.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/fs/compress.hpp"
#include "rummage/fs/erase.hpp"
#include "rummage/fs/folder_icon.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/fs/name_pattern.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

// Temporary drawing/input context; keyboard focus is tracked separately.
struct AreaContext {
    std::size_t& slot;
    std::size_t saved;

    AreaContext(std::size_t& value, std::size_t next)
        : slot{value}, saved{std::exchange(value, next)} {}

    ~AreaContext() { slot = saved; }
};

std::filesystem::path home_directory() {
    const char* const home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): at start
    return home != nullptr && *home == '/' ? std::filesystem::path{home} : std::filesystem::path{};
}

}  // namespace

Browser::Browser(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                 std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                 std::function<void(ui::CursorShape)> set_cursor,
                 std::shared_ptr<const thumbs::Cache> thumbnails)
    : loop_{loop},
      pool_{pool},
      redraw_{redraw},
      set_cursor_{std::move(set_cursor)},
      thumbnails_{thumbnails ? std::make_unique<Thumbnails>(loop, pool, thumbnails, redraw)
                             : nullptr},
      info_pane_{loop, pool, thumbnails_.get(), redraw},
      watcher_{watcher},
      mime_{mime},
      quick_look_{pool, mime, std::move(thumbnails), redraw},
      sidebar_{redraw, resize_cursor(), [this](const fs::Place& place) { open_place(place); }},
      status_bar_{loop, pool, redraw, [this] { open_settings(); }, [this] { shortcuts_.open(); }},
      shortcuts_{redraw},
      editor_{redraw,
              [this](const std::shared_ptr<Folder>& folder, const std::string& from,
                     const std::string& to) { rename(folder, from, to); },
              [this](std::string problem) { show_message(std::move(problem), true); }},
      conflict_{redraw},
      confirm_{redraw},
      password_{redraw},
      mount_password_{redraw},
      location_{redraw},
      batch_rename_{redraw},
      errors_{redraw, [this](std::string text) { copy_text(std::move(text)); }},
      properties_{pool, mime, redraw},
      settings_{pool, redraw},
      menu_{std::move(redraw)},
      palette_{redraw_},
      trash_model_{loop, pool, watcher, std::move(mime)},
      transfers_{loop,
                 [this](const fs::TransferRequest& request, const fs::TransferProgress& progress) {
                     transfer_progress(request, progress);
                 },
                 [this](const fs::Conflict& conflict, std::function<void(fs::Resolution)> answer) {
                     ask(conflict, std::move(answer));
                 },
                 [this](const fs::TransferRequest& request, const fs::TransferResult& result) {
                     transfer_done(request, result);
                 },
                 [this] {
                     refresh_transfer_tasks();
                     redraw_();
                 }} {
    info_pane_.set_copy_handler([this](std::string text) { copy_text(std::move(text)); });
    properties_.set_copy_handler([this](std::string text) { copy_text(std::move(text)); });
    info_pane_.set_insights_handler(
        [this](const std::filesystem::path& path) { show_insights(path); });
    info_pane_.set_code_handler([this](const std::filesystem::path& path) {
        show_insights(path, InsightsView::View::languages);
    });
    areas_[0] = std::make_unique<BrowsingArea>(*this);
    add_pane(0);
    palette_.set_chosen_handler([this](const std::string& id) {
        std::erase(palette_recent_, id);
        palette_recent_.insert(palette_recent_.begin(), id);
        if (palette_recent_.size() > State::kMaxRecent) {
            palette_recent_.resize(State::kMaxRecent);
        }
    });
    palette_.set_close_handler([this] {
        palette_use_ = PaletteUse::commands;
        if (finder_) {
            finder_.reset();  // its paths go, and the memory they took back to the system
            core::give_back_free_memory();
        }
    });
    sidebar_.enable_tree(loop_, pool_, watcher_, mime_);
    sidebar_.set_close_handler([this] { set_sidebar_visible(false); });
    sidebar_.set_logical_expand_handler([this](const std::string& uri) {
        if (!photos_available() || !uri.starts_with("rummage:photos"))
            return;
        prepare_photo_library();
        photo_library_->refresh_navigation();
    });
    sidebar_.set_background_menu(
        [this](float x, float y) { menu_.open(x, y, sidebar_menu(), false); });
    sidebar_.set_eject_handler([this](const fs::Place& place) { eject_place(place); });
    status_bar_.set_cancel_handler([this] { cancel_transfer(); });
    sidebar_.set_menu_handler(
        [this](const fs::Place& place, float x, float y) { place_menu(place, x, y); });
    sidebar_.set_rename_handler(
        [this](const std::filesystem::path& folder, const std::string& label) {
            rename_bookmark(folder, label);
        });
    home_trash_ = fs::home_trash();
    trash_model_.set_change_handler([this](const fs::ModelChange& /*change*/) {
        const bool full = !trash_model_.listing().entries().empty();
        if (full != trash_full_) {
            trash_full_ = full;
            show_places();
        }
    });
    trash_model_.open(fs::trash_path());
}

Pane& Browser::add_pane(std::size_t at) {
    at = std::min(at, area().tabs.size());
    const auto it =
        area().tabs.insert(area().tabs.begin() + static_cast<std::ptrdiff_t>(at),
                           std::make_unique<Pane>(loop_, pool_, watcher_, mime_, redraw_,
                                                  resize_cursor(), thumbnails_.get(), &info_pane_));
    Pane& added = **it;
    const auto wire = [this](auto& view) {
        view.set_open_handler([this] { open_selection(); });
        view.set_edit_slot(&editor_.slot());
        view.set_menu_handler([this](float x, float y) { open_menu(x, y, false); });
        view.set_drop_mark(&drop_mark_);
        view.set_cut_marks(&cut_marks_);
    };
    wire(added.columns);
    wire(added.grid);
    wire(added.details);
    added.insights.set_browser_handlers(
        [this](const std::filesystem::path& path) {
            return std::make_shared<Folder>(loop_, pool_, watcher_, mime_, path, redraw_, true);
        },
        [this, owner = &added] {
            if (&pane() == owner)
                open_selection();
        },
        [this, owner = &added](float x, float y) {
            if (&pane() == owner)
                open_menu(x, y, false);
        },
        &editor_.slot());
    added.insights.set_code_handler(
        [this](auto report) { info_pane_.set_code_report(std::move(report)); });
    added.details.set_header_menu_handler(
        [this](float x, float y, std::optional<DetailsView::Field> field) {
            auto items = details_columns_menu().children;
            if (field && *field != DetailsView::Field::name) {
                const auto& columns = pane().details.columns();
                const auto column_at = std::ranges::find(columns, *field);
                MenuItem left;
                left.id = "column_left";
                left.label = std::string{i18n::tr("Move column left")};
                left.separator = true;
                left.enabled = column_at != columns.end() && column_at > columns.begin() + 1;
                left.action = [this, field] { pane().details.move_column(*field, -1); };
                items.push_back(std::move(left));
                MenuItem right;
                right.id = "column_right";
                right.label = std::string{i18n::tr("Move column right")};
                right.enabled = column_at != columns.end() && column_at + 1 < columns.end();
                right.action = [this, field] { pane().details.move_column(*field, 1); };
                items.push_back(std::move(right));
            }
            menu_.open(x, y, std::move(items), false);
        });
    // A zoom is the tab's: its two lists take the size (the grid has its own).
    Pane* const zoomed = &added;
    const auto lists = [zoomed](std::size_t index) {
        zoomed->columns.set_list_size(index);
        zoomed->details.set_list_size(index);
    };
    added.columns.set_zoom_handler(lists);
    added.details.set_zoom_handler(lists);
    added.details.set_sort_handler([this, zoomed](const fs::SortOrder& order) {
        zoomed->details.set_sort(order.key, order.descending);
        zoomed->columns.set_order(zoomed->details.order());
        zoomed->grid.set_order(zoomed->details.order());
        if (zoomed->photos && photo_library_)
            photo_library_->set_sort(order);
    });
    if (area().tabs.size() > 1) {
        // Inherit settings from the tab this one comes from; later changes stay independent.
        const Pane& from = *area().tabs[at <= area().active ? area().active + 1 : area().active];
        added.details.set_sort(from.details.order().key, from.details.order().descending);
        added.columns.set_order(added.details.order());
        added.grid.set_order(added.details.order());
        added.details.set_widths(from.details.widths());
        added.details.set_columns(from.details.columns());
        added.columns.set_list_size(from.details.list_size());
        added.details.set_list_size(from.details.list_size());
        added.grid.set_size_index(from.grid.size_index());
        added.show_hidden = from.show_hidden;
    }
    added.entry_filter = picker_filter_;
    added.columns.set_entry_filter(added.entry_filter);
    added.grid.set_entry_filter(added.entry_filter);
    added.details.set_entry_filter(added.entry_filter);
    added.columns.set_show_hidden(added.show_hidden);
    added.grid.set_show_hidden(added.show_hidden);
    added.details.set_show_hidden(added.show_hidden);
    added.grid.set_info_visible(info_visible_);
    added.details.set_info_visible(info_visible_);
    return added;
}

void Browser::leave_pane() {
    if (showing_photos())
        photo_library_->cancel_album_chooser();
    clear_type_jump();
    area().breadcrumb.cancel_edit();
    info_pane_.clear_text_focus();
    leave_motion();
    remember_folder(pane());
    filter_focused_ = false;
    quick_look_.close();
    if (menu_.is_open()) {
        menu_.close();
    }
    if (editor_.active()) {
        editor_.confirm();  // as a click elsewhere: the name typed is kept
    }
    press_.reset();
    band_press_.reset();
    end_band();
    if (pointer_target_ == Target::view) {
        view_pointer({.kind = ui::PointerEvent::Kind::leave}, Clock::now());
    }
}

void Browser::enter_pane() {
    note_location();
    if (!pane().settings_folder.empty())
        folder_settings_.remember(pane().settings_folder, folder_settings(pane()), true);
    if (pointer_inside_ && !button_held_) {
        dispatch_pointer({.kind = ui::PointerEvent::Kind::motion, .x = pointer_x_, .y = pointer_y_},
                         Clock::now());
    }
    redraw_();
}

void Browser::new_tab() {
    const std::shared_ptr<Folder> shown = current_folder();
    open_tab(shown && !shown->is_collection() ? shown->path() : home_directory(), true);
}

void Browser::open_tab(const std::filesystem::path& directory, bool show) {
    if (!show) {
        open_tab_behind(directory);
        return;
    }
    note_location();  // where the tab shown was, for its history
    const Mode mode = pane().mode;
    const std::size_t from = area().active;
    leave_pane();
    add_pane(from + 1);
    area().active = from + 1;
    pane().mode = mode;
    open(directory);  // into the new pane: its first step
    enter_pane();
}

void Browser::open_tab_behind(const std::filesystem::path& directory) {
    note_location();
    const Mode mode = pane().mode;
    const std::size_t from = area().active;
    add_pane(from + 1);
    area().active = from + 1;
    pane().mode = mode;
    open(directory);
    area().active = from;
    show_message(i18n::trf("Opened “{}” in a new tab", directory.filename().string()), false);
    redraw_();
}

void Browser::open_tabs(const std::vector<std::filesystem::path>& folders) {
    const std::size_t from = area().active;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        area().active = from + i;  // the next one goes after the last opened
        open_tab_behind(folders[i]);
    }
    area().active = from;
    if (folders.size() > 1) {
        show_message(i18n::trn("Opened {} folder in a new tab", "Opened {} folders in new tabs",
                               folders.size()),
                     false);
    }
}

void Browser::close_tab(std::size_t index) {
    if (area().tabs.size() <= 1 || index >= area().tabs.size()) {
        return;  // the last tab stays: Ctrl+W does nothing then
    }
    remember_folder(*area().tabs[index]);
    const bool shown = index == area().active;
    if (shown) {
        leave_pane();
    }
    const auto& closing = *area().tabs[index];
    if (const auto folder = folder_of(closing)) {
        auto history = closing.history;
        if (closing.here < history.size() && history[closing.here].folder == folder->path()) {
            const auto* cursor = folder->cursor_entry();
            history[closing.here].cursor = cursor ? cursor->name : "";
        }
        area().closed_tabs.push_back({.tab = tab_of(closing, context_area_),
                                      .history = std::move(history),
                                      .here = closing.here,
                                      .index = index});
        if (area().closed_tabs.size() > kMaxClosedTabs)
            area().closed_tabs.erase(area().closed_tabs.begin());
    }
    area().tabs.erase(area().tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (area().active > index || area().active >= area().tabs.size()) {
        --area()
              .active;  // the one after it takes its place, or the one before when it was the last
    }
    if (shown) {
        enter_pane();
    } else {
        redraw_();
    }
}

std::size_t Browser::closed_tab_count() const { return area().closed_tabs.size(); }

void Browser::reopen_tab() {
    if (area().closed_tabs.empty()) {
        show_message(std::string{i18n::tr("No closed tab to reopen")}, false);
        return;
    }
    note_location();
    leave_pane();
    auto closed = std::move(area().closed_tabs.back());
    area().closed_tabs.pop_back();
    const auto index = std::min(closed.index, area().tabs.size());
    add_pane(index);
    area().active = index;
    const auto& tab = closed.tab;
    if (tab.photos) {
        restore_tab(tab, pane().details.list_size(), pane().grid.size_index());
        enter_pane();
        return;
    }
    pane().mode = static_cast<Mode>(tab.mode);
    // The tab's snapshot wins over folder preferences changed since it closed.
    pane().settings_folder = tab.folder.lexically_normal();
    set_list_size(*tab.list_size);
    set_grid_size(grid_index(*tab.grid_icon));
    set_sort(*tab.sort_key, tab.sort_descending);
    pane().details.set_columns(*tab.details_columns);
    pane().details.set_widths(*tab.details_widths);
    set_show_hidden(*tab.show_hidden, false);
    set_name_filter(tab.filter);
    // Open the saved location directly, including in columns. Reconstructing ancestors
    // by selecting through them can stop at a different folder after a rename, filter
    // change or permissions change. Listing here stays asynchronous, and missing or
    // inaccessible locations use the ordinary folder error view.
    open(tab.folder);
    if (const auto folder = current_folder(); folder && !tab.cursor.empty())
        folder->select_name(tab.cursor);
    pane().history = std::move(closed.history);
    pane().here = closed.here;
    enter_pane();
}

void Browser::select_tab(std::size_t index) {
    ++path_navigation_generation_;
    if (index >= area().tabs.size() || index == area().active) {
        return;
    }
    note_location();
    leave_pane();
    area().active = index;
    enter_pane();
}

std::shared_ptr<Folder> Browser::folder_of(const Pane& pane) {
    if (pane.insights.is_open() && pane.insights.folder())
        return pane.insights.folder();
    switch (pane.mode) {
        case Mode::grid:
            return pane.grid.folder();
        case Mode::details:
            return pane.details.folder();
        case Mode::columns:
            break;
    }
    return pane.columns.focused_folder();
}

std::vector<std::string> Browser::tab_titles() const {
    const std::filesystem::path home = home_directory();
    std::vector<std::string> titles;
    titles.reserve(area().tabs.size());
    for (const std::unique_ptr<Pane>& each : area().tabs) {
        const std::shared_ptr<Folder> folder =
            each->mode == Mode::details ? each->details.root_folder() : folder_of(*each);
        if (each->photos) {
            titles.push_back(std::string{i18n::tr("Photo Library")});
        } else if (each->insights.is_open()) {
            titles.push_back(std::format("{} · {}", i18n::tr("Insights"),
                                         each->insights.current_path().filename().string()));
        } else if (!folder) {
            titles.emplace_back();
        } else if (folder->is_trash()) {
            titles.emplace_back(i18n::tr("Trash"));
        } else if (folder->path() == home) {
            titles.emplace_back(i18n::tr("Home"));
        } else if (!folder->path().has_relative_path()) {
            titles.push_back(folder->path().string());  // the root
        } else {
            titles.push_back(folder->path().filename().string());
        }
    }
    return titles;
}

std::vector<std::string> Browser::memory_lines() const {
    const auto mb = [](std::size_t bytes) {
        return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6);
    };
    std::vector<std::string> lines;
    if (finder_) {
        lines.push_back(
            std::format("finding files: {} paths, {}", finder_->entries(), mb(finder_->bytes())));
    }
    if (thumbnails_) {
        const Thumbnails::Usage usage = thumbnails_->usage();
        lines.push_back(
            std::format("thumbnails: {} slots, {} textures {} (budget {}), pixels "
                        "waiting {}",
                        usage.slots, usage.textures, mb(usage.texture_bytes),
                        mb(Thumbnails::kBudgetBytes), mb(usage.pixel_bytes)));
    }
    const QuickLook::Usage look = quick_look_.usage();
    lines.push_back(std::format("quick look: {} slots, textures {}, pixels kept {}", look.slots,
                                mb(look.texture_bytes), mb(look.pixel_bytes)));
    lines.push_back(std::format("glyph atlases: {}", mb(ui::Renderer::atlas_bytes())));
    std::size_t folders = 0;
    std::size_t entries = 0;
    for (std::size_t i = 0; i < pane().columns.column_count(); ++i) {
        ++folders;
        entries += pane().columns.column(i).folder().entries().size();
    }
    lines.push_back(std::format("columns: {} folders, {} entries", folders, entries));
    if (const std::shared_ptr<Folder> shown = current_folder();
        shown && pane().mode != Mode::columns) {
        lines.push_back(std::format("shown folder: {} entries", shown->entries().size()));
    }
    return lines;
}

std::vector<std::string> Browser::targets(const Folder& folder) {
    std::vector<std::string> names = folder.selected_names();
    if (names.empty()) {
        if (const fs::Entry* const cursor = folder.cursor_entry()) {
            names.push_back(cursor->name);
        }
    }
    return names;
}

void Browser::take_selection(bool cut) {
    if (showing_photos()) {
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
            if (row.value("availability", "") != "online") {
                show_message("Some selected originals are unavailable", true);
                return;
            }
            paths.emplace_back(row.at("path").get<std::string>());
        }
        clipboard_ = {.directory = {}, .names = {}, .cut = cut, .paths = paths};
        cut_marks_ = {};
        if (system_.offer) {
            const auto text = path_lines(paths);
            std::vector<SystemClipboard::Format> formats{
                {std::string{kGnomeFiles}, gnome_copied_files(cut, paths)},
                {std::string{kUriList}, uri_list(paths)},
                {std::string{kText}, text},
                {"UTF8_STRING", text},
                {"text/plain", text}};
            if (cut)
                formats.emplace_back(std::string{kKdeCut}, "1");
            system_.offer(std::move(formats));
        }
        show_message(cut ? "Paste in a folder to move the selected originals"
                         : "Paste in a folder to copy the selected originals",
                     false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || (cut && folder->is_archive())) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    const std::size_t count = names.size();
    clipboard_ = {.directory = folder->path(), .names = std::move(names), .cut = cut};
    cut_marks_ = {};
    if (cut) {
        cut_marks_ = {.directory = clipboard_.directory,
                      .names = {clipboard_.names.begin(), clipboard_.names.end()}};
    }
    if (folder->is_archive()) {
        // Claim the clipboard without advertising virtual members as on-disk file URIs.
        if (system_.offer)
            system_.offer(
                {{"text/plain",
                  std::string{i18n::tr("Archive selection: paste into Rummage to extract")}}});
        const auto paste_keys = keys_for(Command::paste);
        show_message(
            i18n::trf("{} extracts the selected archive contents",
                      paste_keys.empty() ? std::string{i18n::tr("Paste")} : i18n::keys(paste_keys)),
            false);
        return;
    }
    if (system_.offer) {
        // Other programs paste them too: file managers, browsers, terminals.
        std::vector<std::filesystem::path> paths;
        paths.reserve(clipboard_.names.size());
        for (const std::string& name : clipboard_.names) {
            paths.push_back(clipboard_.directory / name);
        }
        const std::string lines = path_lines(paths);
        std::vector<SystemClipboard::Format> formats{
            {std::string{kGnomeFiles}, gnome_copied_files(cut, paths)},
            {std::string{kUriList}, uri_list(paths)},
            {std::string{kText}, lines},
            {"UTF8_STRING", lines},
            {"text/plain", lines}};
        if (cut) {
            formats.emplace_back(std::string{kKdeCut}, "1");
        }
        system_.offer(std::move(formats));
    }
    const std::string items = count == 1 ? std::format("“{}”", clipboard_.names.front())
                                         : i18n::trn("{} item", "{} items", count);
    show_message(cut ? i18n::trf("Ctrl+V moves {} where you want them", items)
                     : i18n::trf("Ctrl+V copies {} where you want them", items),
                 false);
}

void Browser::copy_text(std::string text, std::string message) {
    // Text replaces the file clipboard; a later paste can create a text file.
    clipboard_ = {};
    cut_marks_ = {};
    if (message.empty()) {
        const auto lines = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
        message = lines == 1 ? i18n::trn("Copied {} character", "Copied {} characters",
                                         column_at_byte(text, text.size()))
                             : i18n::trn("Copied {} line", "Copied {} lines", lines);
    }
    if (system_.offer) {
        system_.offer({{std::string{kText}, text}, {"UTF8_STRING", text}, {"text/plain", text}});
    }
    show_message(message, false);
}

void Browser::copy_path() {
    if (showing_photos()) {
        const auto rows = photo_library_->selected_photos();
        if (rows.size() != photo_library_->selected_ids().size()) {
            show_message(
                std::string{i18n::tr("Load the selected photos before copying this selection")},
                true);
            return;
        }
        std::vector<std::filesystem::path> paths;
        for (const auto& photo : rows)
            paths.emplace_back(photo.at("path").get<std::string>());
        if (!paths.empty())
            copy_text(path_lines(paths));
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    // The entries' paths are Ctrl+C's: as text, other programs paste them too.
    std::string path = folder->path().string();
    std::string message = i18n::trf("Copied {}", path);
    copy_text(std::move(path), std::move(message));
}

void Browser::paste() {
    if (showing_photos()) {
        show_message(std::string{i18n::tr("Use Show in Folder for filesystem changes; duplicate "
                                          "cleanup is in Photo Library Review")},
                     false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash()) {
        return;
    }
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    if (system_.types && system_.read &&
        (!system_.owned || !system_.owned() || clipboard_.names.empty())) {
        // Another program's: the files it offers, if any.
        const std::vector<std::string> types = system_.types();
        const auto has = [&types](std::string_view type) {
            return std::ranges::find(types, type) != types.end();
        };
        const bool gnome = has(kGnomeFiles);
        if (gnome || has(kUriList)) {
            const std::uint64_t generation = system_.generation ? system_.generation() : 0;
            const auto read_files = [this, gnome, generation,
                                     destination = folder->path()](bool kde_cut) {
                ++operations_;
                system_.read(
                    std::string{gnome ? kGnomeFiles : kUriList},
                    [this, alive = alive_, gnome, kde_cut, generation,
                     destination](const std::optional<std::string>& data) {
                        if (!*alive) {
                            return;
                        }
                        --operations_;
                        std::optional<ClipboardFiles> files;
                        if (data) {
                            files = gnome ? parse_gnome_copied_files(*data) : parse_uri_list(*data);
                        }
                        if (!files) {
                            show_message(std::string{i18n::tr("The clipboard could not be read")},
                                         true);
                            return;
                        }
                        files->cut = files->cut || kde_cut;
                        paste_files(destination, *files, generation);
                    });
            };
            if (!gnome && has(kKdeCut)) {
                // KDE says "cut" in a type of its own.
                ++operations_;
                system_.read(std::string{kKdeCut}, [this, alive = alive_, read_files](
                                                       const std::optional<std::string>& data) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    read_files(data && data->starts_with('1'));
                });
            } else {
                read_files(false);
            }
            return;
        }
        if (const auto type = content_offer(types)) {
            paste_contents(folder, *type);
            return;
        }
        if (!types.empty()) {
            show_message(std::string{i18n::tr("The clipboard holds no files")}, false);
            return;
        }
    }
    if (!clipboard_.paths.empty()) {
        const auto paths = clipboard_.paths;
        const bool cut = clipboard_.cut;
        transfer_paths(folder->path(), paths, cut, std::nullopt);
        if (cut) {
            clipboard_ = {};
            cut_marks_ = {};
            if (system_.clear)
                system_.clear();
        }
        return;
    }
    if (clipboard_.names.empty()) {
        show_message(std::string{i18n::tr("Nothing to paste: Ctrl+C or Ctrl+X first")}, false);
        return;
    }
    const bool cut = clipboard_.cut;
    submit({.kind = cut ? fs::TransferKind::move : fs::TransferKind::copy,
            .source = clipboard_.directory,
            .names = clipboard_.names,
            .destination = folder->path(),
            .force_copy = false},
           open_batch(Purpose::user, cut ? FileOperation::Kind::move : FileOperation::Kind::copy));
    if (cut) {
        clipboard_ = {};  // moved once: there is nothing left to move
        cut_marks_ = {};
        if (system_.clear) {
            system_.clear();
        }
    }
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(
            TransferStatus{.text = std::string{cut ? i18n::tr("Getting ready to move…")
                                                   : i18n::tr("Getting ready to copy…")},
                           .fraction = -1.0F});
    }
    redraw_();
}

void Browser::paste_contents(const std::shared_ptr<Folder>& folder, std::string type) {
    ++operations_;
    system_.read(type, [this, alive = alive_, folder, type](std::optional<std::string> data) {
        if (!*alive)
            return;
        --operations_;
        if (!data) {
            show_message(std::string{i18n::tr("The clipboard could not be read")}, true);
            return;
        }
        create_contents(folder->path(), type, std::move(*data), false, folder);
    });
}

void Browser::create_contents(const std::filesystem::path& directory, std::string type,
                              std::string bytes, bool dropped,
                              const std::shared_ptr<Folder>& reveal) {
    if (fs::is_trash(directory))
        return;
    ++operations_;
    const auto job = ++content_job_id_;
    content_jobs_[job] = pool_.submit(
        core::Priority::visible,
        [directory, type = std::move(type), bytes = std::move(bytes),
         dropped](const core::CancelToken& token) mutable {
            auto extension = content_extension(type, bytes);
            if (dropped &&
                (type == kUriList || std::ranges::find(kTextTypes, type) != kTextTypes.end())) {
                if (const auto url = dropped_web_url(bytes)) {
                    bytes = "[InternetShortcut]\nURL=" + *url + "\n";
                    extension = ".url";
                }
            }
            fs::MadeFile made{.name = {},
                              .error = std::make_error_code(std::errc::invalid_argument)};
            if (bytes.size() > kMaxContentBytes)
                made.error = std::make_error_code(std::errc::file_too_large);
            else if (extension)
                made = fs::make_contents_file(
                    directory,
                    std::string{dropped ? "Dropped" : "Pasted"} + std::string{*extension}, bytes,
                    token);
            std::optional<FileOperation::CreatedFile> created;
            if (!made.error) {
                std::error_code error;
                const auto modified =
                    std::filesystem::last_write_time(directory / made.name, error);
                if (!error)
                    created = FileOperation::CreatedFile{bytes.size(), modified};
            }
            return std::pair{std::move(made), created};
        },
        [this, alive = alive_, directory, reveal, dropped, job](const auto& result) {
            if (!*alive)
                return;
            content_jobs_.erase(job);
            --operations_;
            const auto& [made, created] = result;
            if (made.error) {
                show_message(i18n::trf("Cannot make a file in {}: {}",
                                       directory.filename().string(), made.error.message()),
                             true);
                return;
            }
            record({.kind = FileOperation::Kind::create,
                    .items = {{.from = {}, .to = directory / made.name}},
                    .modes = {},
                    .created_file = created},
                   Purpose::user, false);
            if (reveal && current_folder() == reveal) {
                filter_focused_ = false;
                set_name_filter({});
                reveal->expect_name(made.name);
                if (!dropped && !editor_.active())
                    editor_.start(reveal, made.name, false);
            }
            redraw_();
        });
}

std::uint64_t Browser::open_batch(Purpose purpose, FileOperation::Kind kind) {
    const std::uint64_t id = ++batch_ids_;
    batches_.emplace(
        id, Batch{.purpose = purpose, .kind = kind, .pending = 0, .items = {}, .failed = false});
    return id;
}

void Browser::submit(fs::TransferRequest request, std::uint64_t batch,
                     std::optional<std::uint64_t> empty_clipboard) {
    if (request.kind == fs::TransferKind::copy || request.kind == fs::TransferKind::move)
        request.verify = request.verify || verify_transfers_;
    if (batch_rename_preparing_ || batch_rename_running_) {
        batches_.erase(batch);
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    if (const auto it = batches_.find(batch); it != batches_.end()) {
        ++it->second.pending;
    }
    submitted_.push_back({.batch = batch,
                          .empty_clipboard = empty_clipboard,
                          .motion = capture_operation_motion(request)});
    submitted_.back().id = transfers_.submit(std::move(request));
}

void Browser::paste_files(const std::filesystem::path& destination, const ClipboardFiles& files,
                          std::uint64_t generation) {
    if (fs::is_trash(destination)) {
        return;
    }
    if (files.paths.empty()) {
        show_message(
            std::string{files.refused > 0 ? i18n::tr("Only files on this computer can be pasted")
                                          : i18n::tr("The clipboard holds no files")},
            false);
        return;
    }
    transfer_paths(destination, files.paths, files.cut,
                   files.cut ? std::optional{generation} : std::nullopt);
}

void Browser::transfer_paths(const std::filesystem::path& destination,
                             const std::vector<std::filesystem::path>& paths, bool move,
                             std::optional<std::uint64_t> empty_clipboard) {
    // One transfer per folder the files are in, one operation for Ctrl+Z.
    const std::uint64_t batch =
        open_batch(Purpose::user, move ? FileOperation::Kind::move : FileOperation::Kind::copy);
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> folders;
    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path parent = path.parent_path();
        auto it = std::ranges::find(folders, parent, [](const auto& f) { return f.first; });
        if (it == folders.end()) {
            folders.emplace_back(parent, std::vector<std::string>{});
            it = std::prev(folders.end());
        }
        it->second.push_back(path.filename().string());
    }
    for (auto& [source, names] : folders) {
        submit({.kind = move ? fs::TransferKind::move : fs::TransferKind::copy,
                .source = source,
                .names = std::move(names),
                .destination = destination,
                .force_copy = false},
               batch, empty_clipboard);
    }
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(
            TransferStatus{.text = std::string{move ? i18n::tr("Getting ready to move…")
                                                    : i18n::tr("Getting ready to copy…")},
                           .fraction = -1.0F});
    }
    redraw_();
}

void Browser::paste_text() {
    if (!system_.types || !system_.read) {
        return;
    }
    const std::vector<std::string> types = system_.types();
    const bool for_batch_rename = batch_rename_.is_open();
    const auto batch_rename_generation = batch_rename_.generation();
    const bool for_confirm = confirm_.is_open();
    const auto confirm_generation = confirm_.generation();
    const bool for_password = password_.is_open();
    const auto password_generation = password_.generation();
    const bool for_settings = settings_.is_open();
    const auto settings_generation = settings_.generation();
    const bool for_properties = properties_.is_open();
    const auto properties_generation = properties_.generation();
    const bool for_search =
        searching_results() && search_results_->editing() && !palette_.is_open();
    const auto search_generation = search_generation_;
    const bool for_photos = showing_photos() && photo_library_->editing() && !palette_.is_open();
    const auto photo_generation = for_photos ? photo_library_->edit_generation() : 0;
    const bool for_breadcrumb = area().breadcrumb.editing();
    const auto breadcrumb_generation = area().breadcrumb.edit_generation();
    const auto breadcrumb_area = context_area_;
    const auto* breadcrumb_owner = areas_[breadcrumb_area].get();
    for (const std::string_view type : kTextTypes) {
        if (std::ranges::find(types, type) == types.end()) {
            continue;
        }
        ++operations_;
        system_.read(std::string{type}, [this, alive = alive_, for_confirm, confirm_generation,
                                         for_password, password_generation, for_properties,
                                         properties_generation, for_settings, settings_generation,
                                         for_batch_rename, batch_rename_generation, for_breadcrumb,
                                         breadcrumb_generation, breadcrumb_area, breadcrumb_owner,
                                         for_search, search_generation, for_photos,
                                         photo_generation](const std::optional<std::string>& text) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (!text) {
                return;
            }
            if (for_batch_rename) {
                if (batch_rename_.is_open() &&
                    batch_rename_.generation() == batch_rename_generation)
                    batch_rename_.paste(*text);
                return;
            }
            if (for_confirm) {
                if (confirm_.is_open() && confirm_.generation() == confirm_generation)
                    confirm_.paste(*text);
                return;
            }
            if (for_password) {
                if (password_.is_open() && password_.generation() == password_generation)
                    password_.paste(*text);
                return;
            }
            if (for_settings) {
                if (settings_.is_open() && settings_.generation() == settings_generation)
                    settings_.paste(*text);
                return;
            }
            if (for_properties) {
                if (properties_.is_open() && properties_.generation() == properties_generation)
                    properties_.paste(*text);
                return;
            }
            if (for_photos) {
                if (showing_photos() && photo_library_->edit_generation() == photo_generation)
                    photo_library_->paste(*text);
                return;
            }
            if (for_search) {
                if (searching_results() && search_generation_ == search_generation)
                    search_results_->paste(*text);
                return;
            }
            if (for_breadcrumb) {
                auto* owner = areas_[breadcrumb_area].get();
                if (owner == breadcrumb_owner && owner && owner->breadcrumb.editing() &&
                    owner->breadcrumb.edit_generation() == breadcrumb_generation)
                    owner->breadcrumb.paste(*text);
                return;
            }
            if (mount_password_.is_open()) {
                mount_password_.paste(*text);
            } else if (location_.is_open()) {
                location_.paste(*text);
            } else if (palette_.is_open()) {
                palette_.paste(*text);
            } else if (editor_.active()) {
                editor_.paste(*text);
            } else if (sidebar_.renaming()) {
                sidebar_.paste(*text);
            } else if (filter_focused_) {
                pane().filter_field.insert(
                    std::string_view{*text}.substr(0, text->find_first_of("\r\n")));
                apply_filters();
            } else {
                editor_.paste(*text);
            }
        });
        return;
    }
}

bool Browser::text_field_key(const ui::KeyEvent& event) {
    if (event.modifiers != ui::Modifiers{.ctrl = true})
        return false;
    if (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V) {
        paste_text();
        return true;
    }
    const bool cut = event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X;
    if (!cut && event.keysym != XKB_KEY_c && event.keysym != XKB_KEY_C)
        return false;
    std::string text;
    if (password_.is_open())
        text = password_.copy_selection(cut);
    else if (confirm_.is_open())
        text = confirm_.copy_selection(cut);
    else if (mount_password_.is_open())
        text = mount_password_.copy_selection(cut);
    else if (batch_rename_.is_open())
        text = batch_rename_.copy_selection(cut);
    else if (location_.is_open())
        text = location_.copy_selection(cut);
    else if (palette_.is_open())
        text = cut ? palette_.cut() : palette_.field().selected_text();
    else if (showing_photos() && photo_library_->editing())
        text = photo_library_->copy_selection(cut);
    else if (searching_results() && search_results_->editing())
        text = search_results_->copy_query(cut);
    else if (editor_.active())
        text = cut ? editor_.cut() : editor_.field().selected_text();
    else if (sidebar_.renaming())
        text = cut ? sidebar_.cut() : sidebar_.field().selected_text();
    else if (area().breadcrumb.editing())
        text = area().breadcrumb.copy_selection(cut);
    if (!text.empty())
        copy_text(std::move(text));
    return true;
}

void Browser::clipboard_lost() {
    clipboard_ = {};
    if (!cut_marks_.names.empty()) {
        cut_marks_ = {};
        redraw_();
    }
}

namespace {

// The names going and the darktable sidecars that go with them.
std::unordered_set<std::string> going_with_sidecars(const Folder& folder,
                                                    const std::vector<std::string>& names) {
    std::vector<std::string> listing;
    listing.reserve(folder.entries().size());
    for (const fs::Entry& entry : folder.entries()) {
        listing.push_back(entry.name);
    }
    std::unordered_set<std::string> going{names.begin(), names.end()};
    for (const std::string& name : names) {
        for (std::string& sidecar : fs::sidecars_of(listing, name)) {
            going.insert(std::move(sidecar));
        }
    }
    return going;
}

}  // namespace

void Browser::cursor_after(Folder& folder, const std::unordered_set<std::string>& going) {
    const std::vector<fs::Entry>& entries = folder.entries();
    std::size_t last = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (going.contains(entries[i].name)) {
            last = i;
        }
    }
    std::string next;
    for (std::size_t i = last + 1; i < entries.size() && next.empty(); ++i) {
        if (!going.contains(entries[i].name)) {
            next = entries[i].name;
        }
    }
    for (std::size_t i = last; i-- > 0 && next.empty();) {
        if (!going.contains(entries[i].name)) {
            next = entries[i].name;
        }
    }
    if (next.empty()) {
        folder.set_cursor(std::nullopt);
    } else {
        folder.select_name(std::move(next));
    }
}

void Browser::trash() {
    if (showing_photos()) {
        show_message(std::string{i18n::tr("Use Show in Folder for filesystem changes; duplicate "
                                          "cleanup is in Photo Library Review")},
                     false);
        return;
    }
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->is_archive()) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    // The cursor goes on to the first entry after the last one trashed that stays (else the one
    // before them): their darktable sidecars go too.
    const std::unordered_set<std::string> going = going_with_sidecars(*folder, names);
    submit({.kind = fs::TransferKind::trash,
            .source = folder->path(),
            .names = std::move(names),
            .destination = home_trash_,
            .force_copy = false},
           open_batch(Purpose::user, FileOperation::Kind::trash));
    cursor_after(*folder, going);
    redraw_();
}

void Browser::restore() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || !folder->is_trash()) {
        return;
    }
    const std::vector<std::string> names = targets(*folder);
    const std::unordered_set<std::string> target_names(names.begin(), names.end());
    if (names.empty()) {
        return;
    }
    // The engine is given where the items are; it finds their sidecars itself.
    std::vector<std::string> paths;
    for (const fs::Entry& entry : folder->entries()) {
        if (target_names.contains(entry.name)) {
            paths.push_back(folder->path_of(entry).string());
        }
    }
    submit({.kind = fs::TransferKind::restore,
            .source = folder->path(),
            .names = std::move(paths),
            .destination = {},
            .force_copy = false},
           open_batch(Purpose::user, FileOperation::Kind::restore));
    cursor_after(*folder, going_with_sidecars(*folder, names));
    redraw_();
}

namespace {

// What the card says will go: "With its darktable sidecar · 21.3 MB", "12 items · 412 MB · 1203
// files and folders in all".
std::string erase_detail(const fs::ErasePlan& plan, bool emptying) {
    std::vector<std::string> parts;
    const std::size_t items = plan.names.size();
    if (emptying) {
        parts.push_back(i18n::trn("{} item", "{} items", items));
    } else if (plan.sidecars == 1) {
        parts.emplace_back(items == 1 ? i18n::tr("With its darktable sidecar")
                                      : i18n::tr("With 1 darktable sidecar"));
    } else if (plan.sidecars > 1) {
        parts.push_back(
            i18n::trn("With {} darktable sidecar", "With {} darktable sidecars", plan.sidecars));
    }
    parts.push_back(format_size(plan.bytes));
    if (plan.entries > plan.items.size()) {
        parts.push_back(
            i18n::trn("{} file or folder in all", "{} files and folders in all", plan.entries));
    }
    std::string text;
    for (const std::string& part : parts) {
        text += text.empty() ? part : " · " + part;
    }
    return text;
}

}  // namespace

void Browser::erase() {
    if (showing_photos()) {
        show_message(std::string{i18n::tr("Use Show in Folder for filesystem changes; duplicate "
                                          "cleanup is in Photo Library Review")},
                     false);
        return;
    }
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    std::string title = names.size() == 1 ? i18n::trf("Delete “{}” permanently?", names.front())
                                          : i18n::trn("Delete {} item permanently?",
                                                      "Delete {} items permanently?", names.size());
    fs::TransferRequest request{.kind = fs::TransferKind::erase,
                                .source = folder->path(),
                                .names = {},
                                .destination = {},
                                .force_copy = false};
    if (folder->is_trash()) {
        const std::unordered_set<std::string> target_names(names.begin(), names.end());
        // Where the items are; the engine finds their sidecars itself.
        for (const fs::Entry& entry : folder->entries()) {
            if (target_names.contains(entry.name)) {
                request.names.push_back(folder->path_of(entry).string());
            }
        }
    } else {
        request.names = names;
    }
    ask_erase(
        std::move(request), std::move(title),
        [folder, going = going_with_sidecars(*folder, names)] { cursor_after(*folder, going); });
}

void Browser::empty_trash() {
    ask_erase({.kind = fs::TransferKind::empty_trash,
               .source = fs::trash_path(),
               .names = {},
               .destination = {},
               .force_copy = false},
              std::string{i18n::tr("Empty the trash?")}, nullptr);
}

void Browser::ask_erase(fs::TransferRequest request, std::string title,
                        std::function<void()> after) {
    const std::uint64_t id = ++erase_ids_;
    const bool emptying = request.kind == fs::TransferKind::empty_trash;
    erase_ = PendingErase{.id = id, .request = request, .planned = false, .confirmed = false};
    confirm_.open(std::move(title), std::string{i18n::tr("Adding up the size…")},
                  std::string{emptying ? i18n::tr("Empty") : i18n::tr("Delete")},
                  [this, id, after = std::move(after)](bool confirmed) {
                      if (!erase_ || erase_->id != id) {
                          return;
                      }
                      if (!confirmed) {
                          erase_.reset();  // Escape: nothing goes
                          return;
                      }
                      erase_->confirmed = true;
                      if (after) {
                          after();
                      }
                      if (erase_->planned) {
                          submit_erase();
                      }
                  });
    // The trashes are found on the worker: a disk that does not answer does not stop the loop.
    std::optional<fs::TrashSources> sources;
    if (emptying) {
        sources = fs::trash_sources();
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [request = std::move(request), sources](const core::CancelToken& token) mutable {
            if (sources) {
                for (const fs::TrashDir& trash : fs::existing_trashes(*sources)) {
                    request.names.push_back(trash.path.string());
                }
            }
            fs::ErasePlan plan = fs::plan_erase(request, token);
            return std::pair{std::move(request), std::move(plan)};
        },
        [this, alive = alive_, id,
         emptying](const std::pair<fs::TransferRequest, fs::ErasePlan>& planned) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (!erase_ || erase_->id != id) {
                return;  // given up, or another one asked since
            }
            const auto& [complete, plan] = planned;
            if (plan.names.empty()) {
                confirm_.close();
                erase_.reset();
                if (emptying) {
                    show_message(std::string{i18n::tr("The trash is empty already")}, false);
                } else {
                    show_message(plan.errors.empty() ? std::string{i18n::tr("Nothing to delete")}
                                                     : plan.errors.front(),
                                 !plan.errors.empty());
                }
                return;
            }
            erase_->request = complete;
            erase_->planned = true;
            confirm_.set_detail(erase_detail(plan, emptying));
            if (erase_->confirmed) {
                submit_erase();
            }
        });
}

void Browser::submit_erase() {
    if (!erase_) {
        return;
    }
    const bool emptying = erase_->request.kind == fs::TransferKind::empty_trash;
    submit(std::move(erase_->request), 0);
    erase_.reset();
    undo_ =
        FileOperation{.kind = FileOperation::Kind::erase, .items = {}, .modes = {}};  // no way back
    redo_.reset();
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(TransferStatus{
            .text = std::string{emptying ? i18n::tr("Getting ready to empty the trash…")
                                         : i18n::tr("Getting ready to delete…")},
            .fraction = -1.0F});
    }
    redraw_();
}

void Browser::place_menu(const fs::Place& place, float x, float y, bool keyboard) {
    if (Sidebar::separator(place)) {
        menu_.open(x, y, sidebar_organization_items(place), keyboard);
        return;
    }
    if (Sidebar::child(place)) {
        ++menu_generation_;
        menu_loaders_.clear();
        menu_.open(x, y, configured_menu(place_navigation_items(place), menu_layout_.sidebar),
                   keyboard);
        return;
    }
    if (place.section == fs::Place::Section::devices) {
        ++menu_generation_;
        menu_loaders_.clear();
        std::vector<MenuItem> items = place_navigation_items(place);
        MenuItem mount;
        mount.id = "mount";
        mount.separator = !items.empty();
        mount.label = i18n::tr("Mount");
        mount.enabled = !place.disk.mounted && !place.disk.volume.empty() && bool(devices_.mount);
        mount.action = [this, place] { mount_place(place, false); };
        items.push_back(std::move(mount));
        MenuItem unmount;
        unmount.id = "unmount";
        unmount.label = i18n::tr("Unmount");
        unmount.enabled =
            place.disk.mounted && !place.disk.volume.empty() && bool(devices_.unmount);
        unmount.action = [this, place] { unmount_place(place); };
        items.push_back(std::move(unmount));
        if (place.disk.ejectable) {
            MenuItem eject;
            eject.id = "eject";
            eject.label = i18n::tr("Eject");
            eject.enabled = !place.disk.volume.empty() && bool(devices_.eject);
            eject.separator = true;
            eject.action = [this, place] { eject_place(place); };
            items.push_back(std::move(eject));
        }
        MenuItem properties;
        properties.id = "properties";
        properties.label = i18n::tr("Properties…");
        properties.enabled = place.disk.mounted && !place.path.empty();
        properties.separator = true;
        properties.action = [this, place] {
            open_properties(place.path, false, true, place.label);
        };
        items.push_back(std::move(properties));
        menu_.open(x, y, configured_menu(std::move(items), menu_layout_.sidebar), keyboard);
        return;
    }
    if (place.section == fs::Place::Section::bookmarks) {
        ++menu_generation_;
        menu_loaders_.clear();
        std::vector<MenuItem> items = place_navigation_items(place);
        if (place.uri.empty()) {
            MenuItem rename;
            rename.id = "rename_bookmark";
            rename.label = std::string{i18n::tr("Rename…")};
            rename.keys = "F2";
            rename.separator = !items.empty();
            rename.action = [this, path = place.path] { sidebar_.start_rename(path); };
            items.push_back(std::move(rename));
            if (folder_chooser_) {
                MenuItem choose;
                choose.id = "choose_bookmark_folder";
                choose.label = std::string{i18n::tr("Choose Folder…")};
                choose.action = [this, path = place.path] {
                    folder_chooser_(path,
                                    [this, alive = alive_, path](std::filesystem::path chosen) {
                                        if (*alive) {
                                            repoint_bookmark(path, std::move(chosen));
                                        }
                                    });
                };
                items.push_back(std::move(choose));
            }
        }
        MenuItem remove;
        remove.separator = !items.empty();
        remove.id = "remove_bookmark";
        remove.label = i18n::tr("Remove from Sidebar");
        if (!place.uri.empty()) {
            MenuItem mount;
            mount.id = "mount";
            mount.label = i18n::tr("Mount");
            mount.enabled = !place.disk.mounted && remotes_.mount != nullptr;
            mount.action = [this, place] { mount_remote(place, false); };
            items.push_back(std::move(mount));
            MenuItem unmount;
            unmount.id = "unmount";
            unmount.label = i18n::tr("Unmount");
            unmount.enabled = place.disk.mounted && remotes_.unmount != nullptr;
            unmount.action = [this, place] { unmount_remote(place); };
            items.push_back(std::move(unmount));
            remove.separator = true;
            remove.action = [this, uri = place.uri, label = place.label] {
                remove_remote_from_sidebar(uri, label);
            };
        } else {
            remove.action = [this, path = place.path] { remove_from_sidebar(path); };
        }
        items.push_back(std::move(remove));
        menu_.open(x, y, configured_menu(std::move(items), menu_layout_.sidebar), keyboard);
        return;
    }
    if (!fs::is_trash(place.path)) {
        ++menu_generation_;
        menu_loaders_.clear();
        menu_.open(x, y, configured_menu(place_navigation_items(place), menu_layout_.sidebar),
                   keyboard);
        return;
    }
    ++menu_generation_;
    menu_loaders_.clear();
    MenuItem empty;
    empty.id = "empty_trash";
    empty.label = i18n::tr("Empty Trash…");
    empty.enabled = trash_full_;
    empty.action = [this] { empty_trash(); };
    std::vector<MenuItem> items = place_navigation_items(place);
    items.push_back(std::move(empty));
    menu_.open(x, y, configured_menu(std::move(items), menu_layout_.sidebar), keyboard);
}

void Browser::set_places(std::vector<fs::Place> places) {
    bookmark_icons_job_.cancel();
    places_ = std::move(places);
    show_places();
    if (std::ranges::none_of(places_, [](const fs::Place& place) {
            return place.section == fs::Place::Section::bookmarks && place.uri.empty() &&
                   !place.path.empty();
        }))
        return;
    bookmark_icons_job_ = pool_.submit(
        core::Priority::visible,
        [places = places_, sources = place_sources_](const core::CancelToken& token) mutable {
            const fs::FolderIcons icons{sources};
            for (auto& place : places) {
                if (token.cancelled())
                    break;
                if (place.section == fs::Place::Section::bookmarks && place.uri.empty() &&
                    !place.path.empty())
                    place.folder_icon = icons.for_folder(place.path);
            }
            return places;
        },
        [this](std::vector<fs::Place> resolved) {
            places_ = std::move(resolved);
            show_places();
        });
}

void Browser::show_places() {
    std::vector<fs::Place> places = places_;
    Sidebar::LogicalGroups photo_groups;
    for (fs::Place& place : places) {
        if (fs::is_trash(place.path)) {
            place.icon = trash_full_ ? "trash-filled" : "trash";
        }
    }
    if (!saved_searches_.empty() && !picker_mode_) {
        const auto end_places = std::ranges::find_if(
            places, [](const auto& place) { return place.section != fs::Place::Section::places; });
        places.insert(end_places, fs::Place{.section = fs::Place::Section::places,
                                            .label = std::string{i18n::tr("Saved Searches…")},
                                            .path = {},
                                            .icon = "search",
                                            .uri = "rummage:saved-searches"});
    }
    if (photos_available()) {
        const auto end_places = std::ranges::find_if(
            places, [](const auto& place) { return place.section != fs::Place::Section::places; });
        places.insert(end_places, fs::Place{.section = fs::Place::Section::places,
                                            .label = std::string{i18n::tr("Photo Library")},
                                            .path = {},
                                            .icon = "photo",
                                            .uri = "rummage:photos"});
        const auto logical = [](std::string label, std::string_view icon, std::string uri) {
            return fs::Place{.section = fs::Place::Section::places,
                             .label = std::move(label),
                             .path = {},
                             .icon = icon,
                             .uri = std::move(uri)};
        };
        photo_groups["rummage:photos"] = {
            .children = {logical(std::string{i18n::tr("Albums")}, "folder",
                                 "rummage:photos/albums"),
                         logical(std::string{i18n::tr("Sources")}, "folder",
                                 "rummage:photos/sources"),
                         logical(std::string{i18n::tr("Saved searches")}, "bookmark",
                                 "rummage:photos/searches"),
                         logical(std::string{i18n::tr("Review duplicates")}, "files",
                                 "rummage:photos/review"),
                         logical(std::string{i18n::tr("Activity")}, "list-details",
                                 "rummage:photos/activity")},
            .expanded_by_default = true};
        for (const auto& [key, uri, icon] :
             {std::tuple{"albums", "rummage:photos/albums", "photo"},
              std::tuple{"sources", "rummage:photos/sources", "folder"},
              std::tuple{"filters", "rummage:photos/searches", "bookmark"}}) {
            auto& group = photo_groups[uri];
            for (const auto& record : photo_navigation_.value(key, nlohmann::json::array())) {
                auto label = record.value("name", "");
                if (std::string_view{key} == "sources") {
                    const std::filesystem::path path{record.value("path", "")};
                    label = path.filename().empty() ? path.string() : path.filename().string();
                }
                group.children.push_back(logical(
                    std::move(label), icon,
                    std::string{uri} + "/" + std::to_string(record.value("id", std::int64_t{}))));
            }
        }
    }
    sidebar_.set_logical_groups(std::move(photo_groups));
    sidebar_.set_places(std::move(places));
}

void Browser::cancel_transfer() {
    transfers_.cancel();
    conflict_.close();
}

void Browser::cancel_transfer_task(std::uint64_t id) {
    const auto tasks = transfers_.tasks();
    const auto task = std::ranges::find(tasks, id, &fs::TransferTask::id);
    const bool active = task != tasks.end() && task->started && !task->finished();
    if (transfers_.cancel(id) && active) {
        conflict_.close();
        // Archive-size confirmation is the other question a running transfer can own.
        if (task->request.from_archive && confirm_.is_open())
            confirm_.close();
    }
}

void Browser::ask(const fs::Conflict& conflict, std::function<void(fs::Resolution)> answer) {
    if (!conflict.question.empty()) {
        confirm_.open(
            std::string{i18n::tr("Extract this archive?")}, conflict.question,
            std::string{i18n::tr("Extract")},
            [answer = std::move(answer)](bool yes) {
                answer(yes ? fs::Resolution::replace : fs::Resolution::cancel);
            },
            std::string{i18n::tr("The contents are much larger than the compressed archive.")});
        return;
    }
    if (for_the_rest_ &&
        (*for_the_rest_ != fs::Resolution::replace ||
         (conflict.directory == conflict.target_directory &&
          conflict.directory == for_the_rest_folder_ && (!conflict.directory || conflict.merge)))) {
        answer(*for_the_rest_);
        return;
    }
    copy_started_.reset();  // the time spent on the question does not count in the rate
    conflict_.open(conflict, conflict.destination.filename().string(),
                   [this, folder = conflict.directory, answer = std::move(answer)](
                       fs::Resolution resolution, bool rest) {
                       if (rest) {
                           for_the_rest_ = resolution;
                           for_the_rest_folder_ = folder;
                       }
                       answer(resolution);
                   });
}

namespace {

// "8 s", "2 min", "1 h 5 min".
std::string format_duration(double seconds) {
    const long whole = std::lround(seconds);
    if (whole < 60) {
        return std::format("{} s", std::max(whole, 1L));
    }
    if (whole < 3600) {
        return std::format("{} min", (whole + 30) / 60);
    }
    return std::format("{} h {} min", whole / 3600, (whole % 3600) / 60);
}

}  // namespace

void Browser::transfer_progress(const fs::TransferRequest& request,
                                const fs::TransferProgress& progress) {
    const bool copy = request.kind == fs::TransferKind::copy;
    const std::size_t items = request.names.size();
    TransferStatus status;
    if (request.kind == fs::TransferKind::restore) {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text = i18n::trn("Restoring {1} of {0} item", "Restoring {1} of {0} items",
                                progress.files_total, progress.files_done);
    } else if (request.kind == fs::TransferKind::trash) {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text =
            i18n::trn("Moving {1} of {0} item to the trash", "Moving {1} of {0} items to the trash",
                      progress.files_total, progress.files_done);
    } else if (request.kind == fs::TransferKind::erase ||
               request.kind == fs::TransferKind::empty_trash) {
        const bool emptying = request.kind == fs::TransferKind::empty_trash;
        if (progress.preparing) {
            status.text = std::string{emptying ? i18n::tr("Getting ready to empty the trash…")
                                               : i18n::tr("Getting ready to delete…")};
        } else {
            status.fraction = progress.files_total > 0
                                  ? static_cast<float>(progress.files_done) /
                                        static_cast<float>(progress.files_total)
                                  : -1.0F;
            status.text = emptying ? i18n::trn("Emptying the trash: {1} of {0} file or folder",
                                               "Emptying the trash: {1} of {0} files and folders",
                                               progress.files_total, progress.files_done)
                                   : i18n::trn("Deleting {1} of {0} file or folder",
                                               "Deleting {1} of {0} files and folders",
                                               progress.files_total, progress.files_done);
        }
    } else if (request.kind == fs::TransferKind::link) {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text = i18n::trf("Making {} of {} links", progress.files_done, progress.files_total);
    } else if (request.kind == fs::TransferKind::compress) {
        status.fraction = progress.bytes_total > 0
                              ? static_cast<float>(static_cast<double>(progress.bytes_done) /
                                                   static_cast<double>(progress.bytes_total))
                              : -1.0F;
        status.text =
            progress.preparing
                ? std::string{i18n::tr("Getting ready to compress…")}
                : i18n::trf("Compressing into “{}” · {} of {}", request.archive_name,
                            format_size(progress.bytes_done), format_size(progress.bytes_total));
    } else if (progress.preparing) {
        if (copy) {
            status.text = i18n::trn("Getting ready to copy {} item…",
                                    "Getting ready to copy {} items…", items);
        } else {
            status.text = i18n::trn("Getting ready to move {} item…",
                                    "Getting ready to move {} items…", items);
        }
    } else if (request.verify && !request.from_archive &&
               (progress.bytes_total > 0 || progress.verifying)) {
        status.fraction =
            progress.bytes_total > 0
                ? static_cast<float>((static_cast<double>(progress.bytes_done) +
                                      static_cast<double>(progress.bytes_verified)) /
                                     (2.0 * static_cast<double>(progress.bytes_total)))
                : -1.0F;
        status.text =
            i18n::trf("Copied {} of {} · verified {}", format_size(progress.bytes_done),
                      format_size(progress.bytes_total), format_size(progress.bytes_verified));
        if (progress.verifying)
            status.text = std::string{i18n::tr("Verifying…")} + " " + status.text;
    } else if (progress.bytes_total > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!copy_started_) {
            copy_started_ = RateStart{.time = now, .bytes = progress.bytes_done};
        }
        status.fraction = static_cast<float>(static_cast<double>(progress.bytes_done) /
                                             static_cast<double>(progress.bytes_total));
        const std::string done = format_size(progress.bytes_done);
        const std::string total = format_size(progress.bytes_total);
        if (copy) {
            status.text = i18n::trn("Copying {} file · {} of {}", "Copying {} files · {} of {}",
                                    progress.files_total, done, total);
        } else {
            status.text = i18n::trn("Moving {} file · {} of {}", "Moving {} files · {} of {}",
                                    progress.files_total, done, total);
        }
        const double seconds = std::chrono::duration<double>(now - copy_started_->time).count();
        if (seconds >= 1.0 && progress.bytes_done > copy_started_->bytes) {
            const double rate =
                static_cast<double>(progress.bytes_done - copy_started_->bytes) / seconds;
            status.text += i18n::trf(
                " · {}/s · {} left", format_size(static_cast<std::uint64_t>(rate)),
                format_duration(static_cast<double>(progress.bytes_total - progress.bytes_done) /
                                rate));
        }
    } else {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text = i18n::trn("Moving {1} of {0} item", "Moving {1} of {0} items",
                                progress.files_total, progress.files_done);
    }
    if (transfers_.pending() > 1) {
        status.text += i18n::trf(" · {} more waiting", transfers_.pending() - 1);
    }
    status_bar_.set_transfer(std::move(status));
}

void Browser::transfer_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    photo_transfer_done(request, result);
    if (searching_results())
        search_results_->refresh();
    Submitted submitted;
    if (const auto found = std::ranges::find(submitted_, request.id, &Submitted::id);
        found != submitted_.end()) {
        submitted = *found;
        submitted_.erase(found);
    }
    complete_operation_motion(submitted.motion, request, result);
    // Another program's cut that moved something: nobody should move those files again. Unless
    // the clipboard holds something else by now.
    if (submitted.empty_clipboard && !result.placed.empty() && system_.clear &&
        system_.generation && system_.generation() == *submitted.empty_clipboard) {
        system_.clear();
    }
    if (!result.cancelled_before_start)
        report_done(request, result);
    else if (transfers_.pending() == 0)
        status_bar_.set_transfer(std::nullopt);
    const auto batch = batches_.find(submitted.batch);
    if (batch == batches_.end()) {
        return;
    }
    Batch& done = batch->second;
    done.items.insert(done.items.end(), result.items.begin(), result.items.end());
    done.undo_excluded += result.undo_excluded;
    if (request.kind == fs::TransferKind::group) {
        done.group_folder = result.group_folder;
        done.grouped = !request.ungroup;
    }
    done.failed =
        done.failed || result.cancelled || !result.errors.empty() || !result.refused.empty();
    if (--done.pending == 0) {
        Batch finished = std::move(done);
        batches_.erase(batch);
        record({.kind = finished.kind,
                .items = std::move(finished.items),
                .undo_excluded = finished.undo_excluded,
                .group_folder = std::move(finished.group_folder),
                .grouped = finished.grouped},
               finished.purpose, finished.failed);
    }
}

void Browser::report_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    for_the_rest_.reset();
    copy_started_.reset();
    conflict_.close();
    if (transfers_.pending() == 0) {
        status_bar_.set_transfer(std::nullopt);
    }
    if (request.kind == fs::TransferKind::trash) {
        trash_done(result);
        return;
    }
    if (request.kind == fs::TransferKind::restore) {
        restore_done(result);
        return;
    }
    if (request.kind == fs::TransferKind::erase || request.kind == fs::TransferKind::empty_trash) {
        erase_done(request, result);
        return;
    }
    if (request.kind == fs::TransferKind::link) {
        links_done(request, result);
        return;
    }
    if (request.kind == fs::TransferKind::compress) {
        compress_done(request, result);
        return;
    }
    const bool copy = request.kind == fs::TransferKind::copy;
    const std::size_t files = result.files_done;
    const std::string what = i18n::trn("{} file", "{} files", files);
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(request.from_archive ? i18n::trf("Stopped: {} extracted so far", what)
                     : copy               ? i18n::trf("Stopped: {} copied so far", what)
                                          : i18n::trf("Stopped: {} moved so far", what),
                     false);
    } else if (!result.errors.empty()) {
        show_message(request.from_archive ? i18n::trf("Extracted {}; {} could not be: {}", what,
                                                      result.errors.size(), result.errors.front())
                     : copy               ? i18n::trf("Copied {}; {} could not be: {}", what,
                                                      result.errors.size(), result.errors.front())
                            : i18n::trf("Moved {}; {} could not be: {}", what, result.errors.size(),
                                        result.errors.front()),
                     true);
    } else if (result.placed.empty()) {
        show_message(std::string{i18n::tr("Nothing to do: every name was skipped")}, false);
    } else if (result.files_verified > 0) {
        show_message(
            i18n::trf("Transfer complete · verified regular files: {}", result.files_verified),
            false);
    } else if (result.bytes_done > 0) {
        const std::string size = format_size(result.bytes_done);
        show_message(request.from_archive ? i18n::trf("Extracted {} ({})", what, size)
                     : copy               ? i18n::trf("Copied {} ({})", what, size)
                                          : i18n::trf("Moved {} ({})", what, size),
                     false);
    } else {
        show_message(request.from_archive ? i18n::trf("Extracted {}", what)
                     : copy               ? i18n::trf("Copied {}", what)
                                          : i18n::trf("Moved {}", what),
                     false);
    }
    if (request.kind == fs::TransferKind::group && !request.ungroup && !result.items.empty()) {
        if (const auto folder = current_folder();
            folder && folder->path() == request.destination.parent_path())
            folder->expect_names({request.destination.filename().string()});
    }
    // What arrived is selected, when its folder is the one shown.
    if (const std::shared_ptr<Folder> folder = current_folder();
        folder && folder->path() == request.destination && !result.placed.empty()) {
        folder->expect_names(result.placed);
    }
}

void Browser::trash_done(const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    std::string what = items == 1 ? std::format("“{}”", result.placed.front())
                                  : i18n::trn("{} item", "{} items", items);
    if (const std::size_t sidecars = result.files_done - items; sidecars > 0) {
        what += i18n::trn(" with {} darktable sidecar", " with {} darktable sidecars", sidecars);
    }
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(i18n::trf("Stopped: {} moved to the trash so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(i18n::trf("{} moved to the trash; {} could not be: {}", what,
                               result.errors.size(), result.errors.front()),
                     true);
    } else if (items > 0) {
        show_message(i18n::trf("{} moved to the trash", what), false);
    }
}

void Browser::restore_done(const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    std::string what = items == 1 ? std::format("“{}”", result.placed.front())
                                  : i18n::trn("{} item", "{} items", items);
    if (const std::size_t sidecars = result.files_done - items; sidecars > 0) {
        what += i18n::trn(" with {} darktable sidecar", " with {} darktable sidecars", sidecars);
    }
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(i18n::trf("Stopped: {} restored so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(i18n::trf("Restored {}; {} could not be: {}", what, result.errors.size(),
                               result.errors.front()),
                     true);
    } else if (items == 0) {
        show_message(std::string{i18n::tr("Nothing restored: every name was skipped")}, false);
    } else {
        show_message(i18n::trf("Restored {}", what), false);
    }
}

void Browser::erase_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    const std::string what = items == 1 ? std::format("“{}”", result.placed.front())
                                        : i18n::trn("{} item", "{} items", items);
    const std::string size =
        result.bytes_done > 0 ? i18n::trf(" ({})", format_size(result.bytes_done)) : "";
    if (result.cancelled) {
        show_message(i18n::trf("Stopped: {} deleted so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(i18n::trf("Deleted {}; {} could not be: {}", what, result.errors.size(),
                               result.errors.front()),
                     true);
    } else if (request.kind == fs::TransferKind::empty_trash) {
        show_message(i18n::trf("Emptied the trash: {} deleted{}", what, size), false);
    } else {
        show_message(i18n::trf("Deleted {} permanently{}", what, size), false);
    }
}

namespace {

// A new folder or file as it was made: an empty folder, or a file as long as its template (or
// empty). Only then does Ctrl+Z take it away.
bool unchanged_since_made(const fs::PlacedItem& item,
                          const std::optional<FileOperation::CreatedFile>& created) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(item.to, error);
    if (error) {
        return false;
    }
    if (std::filesystem::is_directory(status)) {
        return std::filesystem::is_empty(item.to, error) && !error;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(item.to, error);
    if (error) {
        return false;
    }
    if (created) {
        const auto modified = std::filesystem::last_write_time(item.to, error);
        return !error && size == created->size && modified == created->modified;
    }
    if (item.from.empty()) {
        return size == 0;
    }
    const std::uintmax_t made_from = std::filesystem::file_size(item.from, error);
    return !error && size == made_from;
}

}  // namespace

namespace {

std::string cannot_undo(std::size_t undo_excluded) {
    return i18n::trn("{} item cannot be safely undone", "{} items cannot be safely undone",
                     undo_excluded);
}

}  // namespace

void Browser::record(FileOperation operation, Purpose purpose, bool failed) {
    photo_reconcile(operation);
    const std::size_t undo_excluded = std::exchange(undo_excluded_, 0);
    if (purpose != Purpose::user) {
        undoing_ = false;
    }
    if (operation.items.empty() && operation.undo_excluded == 0) {
        return;  // nothing was done: what Ctrl+Z undoes stays
    }
    const std::string what = describe(operation);
    switch (purpose) {
        case Purpose::user:
            undo_ = std::move(operation);
            redo_.reset();
            return;
        case Purpose::undo:
            redo_ = std::move(operation);
            undo_.reset();
            break;
        case Purpose::redo:
            undo_ = std::move(operation);
            redo_.reset();
            break;
    }
    if (!failed) {  // otherwise the transfer's own message says what went wrong
        std::string message = purpose == Purpose::undo ? i18n::trf("Undone: {}", what)
                                                       : i18n::trf("Redone: {}", what);
        if (purpose == Purpose::undo && undo_excluded > 0) {
            message += "; " + cannot_undo(undo_excluded);
        }
        show_message(message, false);
    }
}

void Browser::undo() {
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    if (undoing_) {
        show_message(std::string{i18n::tr("Still undoing the last one")}, false);
    } else if (!undo_) {
        show_message(std::string{i18n::tr("Nothing to undo")}, false);
    } else {
        if (undo_->items.empty() && undo_->undo_excluded > 0) {
            show_message(cannot_undo(undo_->undo_excluded), false);
            undo_.reset();
            return;
        }
        undo_excluded_ = undo_->undo_excluded;
        start_inverse(*undo_, Purpose::undo);
    }
}

void Browser::redo() {
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    if (undoing_) {
        show_message(std::string{i18n::tr("Still undoing the last one")}, false);
    } else if (!redo_) {
        show_message(std::string{i18n::tr("Nothing to redo")}, false);
    } else {
        start_inverse(*redo_, Purpose::redo);
    }
}

Browser::RunningTargets Browser::running_targets(const Folder& folder) const {
    RunningTargets found;
    if (picker_mode_ || folder.is_trash() || folder.is_archive()) {
        return found;
    }
    const std::vector<std::string> names = targets(folder);
    const std::unordered_set<std::string> target_names(names.begin(), names.end());
    bool all = true;
    bool any = false;
    bool runnable = true;
    for (const fs::Entry& entry : folder.entries()) {
        if (entry.kind != fs::EntryKind::file || !target_names.contains(entry.name)) {
            continue;
        }
        found.files.push_back(folder.path() / entry.name);
        const bool runs = (entry.mode & S_IXUSR) != 0;
        all = all && runs;
        any = any || runs;
        runnable = runnable && folder.type_runs(entry);
    }
    // Folders among them: not this entry (a folder's execute bit means something else).
    if (found.files.size() != names.size() || (!any && !runnable)) {
        found.files.clear();
    }
    found.all_run = !found.files.empty() && all;
    return found;
}

void Browser::set_running(bool allow) {
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    RunningTargets found = running_targets(*folder);
    if (found.files.empty()) {
        return;
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [files = std::move(found.files), allow](const core::CancelToken& /*token*/) {
            return fs::set_running(files, allow);
        },
        [this, alive = alive_](const fs::ModeResult& result) {
            if (*alive) {
                --operations_;
                modes_done(result, Purpose::user);
            }
        });
}

void Browser::modes_done(const fs::ModeResult& result, Purpose purpose) {
    FileOperation operation{.kind = FileOperation::Kind::permissions, .items = {}, .modes = {}};
    for (const fs::ModeChange& change : result.changed) {
        operation.items.push_back({.from = change.path, .to = change.path});
    }
    operation.modes = result.changed;
    const bool failed = !result.errors.empty();
    if (purpose == Purpose::user && !failed && !operation.items.empty()) {
        show_message(describe(operation), false);
    }
    record(std::move(operation), purpose, failed);
    if (failed) {
        show_message(
            result.errors.size() == 1
                ? result.errors.front()
                : std::format("{} and {} more", result.errors.front(), result.errors.size() - 1),
            true);
    }
}

void Browser::ask_to_run(const std::filesystem::path& program,
                         std::function<void()> allow_and_run) {
    const std::string name = program.filename().string();
    confirm_.open(
        i18n::trf("Allow \u201c{}\u201d to run?", name),
        std::string{i18n::tr("It is a program, and it is not allowed to run yet.")},
        std::string{i18n::tr("Allow and Run")},
        [allow_and_run = std::move(allow_and_run)](bool confirmed) {
            if (confirmed) {
                allow_and_run();
            }
        },
        std::string{i18n::tr("Run only programs you trust: they run with your permissions.")});
    redraw_();
}

void Browser::trash_paths(const std::vector<std::filesystem::path>& paths, Purpose purpose) {
    const std::uint64_t batch = open_batch(purpose, FileOperation::Kind::trash);
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> folders;
    for (const std::filesystem::path& path : paths) {
        auto it = std::ranges::find(folders, path.parent_path(),
                                    [](const auto& folder) { return folder.first; });
        if (it == folders.end()) {
            folders.emplace_back(path.parent_path(), std::vector<std::string>{});
            it = std::prev(folders.end());
        }
        it->second.push_back(path.filename().string());
    }
    if (purpose != Purpose::user) {
        undoing_ = true;
    }
    for (auto& [source, names] : folders) {
        submit({.kind = fs::TransferKind::trash,
                .source = source,
                .names = std::move(names),
                .destination = home_trash_,
                .force_copy = false},
               batch);
    }
}

void Browser::start_inverse(const FileOperation& operation, Purpose purpose) {
    std::vector<std::filesystem::path> placed;
    placed.reserve(operation.items.size());
    for (const fs::PlacedItem& item : operation.items) {
        placed.push_back(item.to);
    }
    switch (operation.kind) {
        case FileOperation::Kind::group: {
            if (operation.items.empty())
                return;
            fs::TransferRequest request;
            request.kind = fs::TransferKind::group;
            request.source = operation.items.front().to.parent_path();
            request.destination = operation.items.front().from.parent_path();
            request.group_folder = operation.group_folder;
            request.ungroup = operation.grouped;
            for (const auto& item : operation.items) {
                request.names.push_back(item.to.filename().string());
                request.destination_names.push_back(item.from.filename().string());
            }
            undoing_ = true;
            submit(std::move(request), open_batch(purpose, FileOperation::Kind::group));
            return;
        }
        case FileOperation::Kind::erase:
            show_message(std::string{i18n::tr("A permanent delete cannot be undone")}, false);
            return;
        case FileOperation::Kind::permissions:
            undoing_ = true;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [modes = operation.modes](const core::CancelToken& /*token*/) {
                    return fs::restore_modes(modes, true);
                },
                [this, alive = alive_, purpose](const fs::ModeResult& result) {
                    if (*alive) {
                        --operations_;
                        modes_done(result, purpose);
                    }
                });
            return;
        case FileOperation::Kind::batch_rename: {
            auto changes = operation.renames;
            for (auto& change : changes) {
                std::swap(change.from, change.to);
            }
            if (!operation.items.empty()) {
                apply_batch_rename(operation.items.front().to.parent_path(), std::move(changes),
                                   purpose);
            }
            return;
        }
        case FileOperation::Kind::rename: {
            const fs::PlacedItem item = operation.items.front();
            undoing_ = true;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [item](const core::CancelToken& /*token*/) {
                    return fs::rename_entry(item.to.parent_path(), item.to.filename().string(),
                                            item.from.filename().string());
                },
                [this, alive = alive_, item, purpose](const fs::Renamed& renamed) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    if (renamed.error) {
                        undoing_ = false;
                        show_message(renamed.problem, true);
                        return;
                    }
                    record({.kind = FileOperation::Kind::rename,
                            .items = {{.from = item.to, .to = item.from}},
                            .modes = {}},
                           purpose, !renamed.problem.empty());
                    if (!renamed.problem.empty()) {
                        show_message(renamed.problem, true);
                    }
                });
            return;
        }
        case FileOperation::Kind::create: {
            const fs::PlacedItem item = operation.items.front();
            undoing_ = true;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [item, created = operation.created_file](const core::CancelToken& /*token*/) {
                    return unchanged_since_made(item, created);
                },
                [this, alive = alive_, item, purpose](bool unchanged) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    if (!unchanged) {
                        undoing_ = false;
                        show_message(i18n::trf("“{}” has changed since it was made: not undone",
                                               item.to.filename().string()),
                                     false);
                        return;
                    }
                    trash_paths({item.to}, purpose);
                });
            return;
        }
        case FileOperation::Kind::copy:
        case FileOperation::Kind::restore:
            trash_paths(placed, purpose);  // copies go to the trash: an undo can be undone
            return;
        case FileOperation::Kind::trash: {
            std::vector<std::string> paths;
            paths.reserve(placed.size());
            for (const std::filesystem::path& path : placed) {
                paths.push_back(path.string());
            }
            undoing_ = true;
            submit({.kind = fs::TransferKind::restore,
                    .source = fs::trash_path(),
                    .names = std::move(paths),
                    .destination = {},
                    .force_copy = false},
                   open_batch(purpose, FileOperation::Kind::restore));
            return;
        }
        case FileOperation::Kind::move: {
            // Back where each came from, one transfer per pair of folders.
            const std::uint64_t batch = open_batch(purpose, FileOperation::Kind::move);

            struct MoveBack {
                std::pair<std::filesystem::path, std::filesystem::path> folders;
                std::vector<std::string> names;
                std::vector<std::string> destination_names;
            };

            std::vector<MoveBack> moves;
            for (const fs::PlacedItem& item : operation.items) {
                const std::pair folders{item.to.parent_path(), item.from.parent_path()};
                auto it = std::ranges::find(moves, folders, &MoveBack::folders);
                if (it == moves.end()) {
                    moves.push_back({.folders = folders, .names = {}, .destination_names = {}});
                    it = std::prev(moves.end());
                }
                it->names.push_back(item.to.filename().string());
                it->destination_names.push_back(item.from.filename().string());
            }
            undoing_ = true;
            for (auto& move : moves) {
                submit({.kind = fs::TransferKind::move,
                        .source = move.folders.first,
                        .names = std::move(move.names),
                        .destination = move.folders.second,
                        .force_copy = false,
                        .make_destination = true,
                        .destination_names = std::move(move.destination_names)},
                       batch);
            }
            return;
        }
    }
}

Browser::~Browser() {
    for (const auto& [id, token] : content_jobs_)
        token.cancel();
    loop_.remove(type_jump_timer_);
    drive_hover_job_.cancel();
    bookmark_icons_job_.cancel();
    *alive_ = false;
    loop_.remove(auto_scroll_timer_);
    loop_.remove(band_timer_);
    loop_.remove(control_hint_timer_);
}

void Browser::start_rename() {
    if (showing_photos()) {
        show_message(std::string{i18n::tr("Use Show in Folder for filesystem changes; duplicate "
                                          "cleanup is in Photo Library Review")},
                     false);
        return;
    }
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->is_trash() || folder->is_archive()) {
        return;
    }
    auto names = folder->selected_names();
    if (names.size() > 1) {
        if (!picker_mode_) {
            start_batch_rename(folder, std::move(names));
        }
        return;
    }
    const fs::Entry* const entry = folder->cursor_entry();
    if (entry == nullptr) {
        return;
    }
    editor_.start(folder, entry->name, entry->is_directory());
}

void Browser::new_folder() {
    if (showing_photos()) {
        photo_library_->add_folder();
        return;
    }
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->is_trash() || folder->is_archive()) {
        return;
    }
    // Keep the new folder visible so its name can be edited immediately.
    filter_focused_ = false;
    if (!name_filter().empty()) {
        set_name_filter({});
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path()](const core::CancelToken& /*token*/) {
            return fs::make_folder(directory, std::string{i18n::tr("New Folder")});
        },
        [this, alive = alive_, folder](const fs::MadeFolder& made) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (made.error) {
                show_message(i18n::trf("Cannot make a folder in {}: {}",
                                       folder->path().filename().string(), made.error.message()),
                             true);
                return;
            }
            folder->expect_name(made.name);  // lands when the watcher reports it
            record({.kind = FileOperation::Kind::create,
                    .items = {{.from = {}, .to = folder->path() / made.name}},
                    .modes = {}},
                   Purpose::user, false);
            if (current_folder() == folder && !editor_.active()) {
                editor_.start(folder, made.name, true);
            }
        });
}

void Browser::new_file(const std::string& name, const std::filesystem::path& from) {
    if (showing_photos())
        return;
    if (batch_rename_preparing_ || batch_rename_running_) {
        show_message(std::string{i18n::tr("Wait for Batch Rename to finish")}, false);
        return;
    }
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->is_archive()) {
        return;
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path(), name, from](const core::CancelToken& /*token*/) {
            return fs::make_file(directory, name, from);
        },
        [this, alive = alive_, folder, from](const fs::MadeFile& made) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (made.error) {
                show_message(i18n::trf("Cannot make a file in {}: {}",
                                       folder->path().filename().string(), made.error.message()),
                             true);
                return;
            }
            folder->expect_name(made.name);  // lands when the watcher reports it
            record({.kind = FileOperation::Kind::create,
                    .items = {{.from = from, .to = folder->path() / made.name}},
                    .modes = {}},
                   Purpose::user, false);
            if (current_folder() == folder && !editor_.active()) {
                editor_.start(folder, made.name, false);
            }
        });
}

std::vector<MenuItem> Browser::entry_menu(const std::shared_ptr<Folder>& folder) {
    const std::vector<std::string> names = targets(*folder);
    const std::unordered_set<std::string> target_names(names.begin(), names.end());
    const fs::Entry* const cursor = folder->cursor_entry();
    std::vector<MenuItem> items;
    if (folder->is_trash()) {
        MenuItem back;
        back.id = "restore";
        back.label = i18n::tr("Restore");
        back.action = [this] { restore(); };
        items.push_back(std::move(back));
        MenuItem gone;
        gone.id = "erase";
        gone.label = i18n::tr("Delete Permanently…");
        gone.keys = keys_for(Command::trash);
        gone.separator = true;
        gone.action = [this] { erase(); };
        items.push_back(std::move(gone));
        MenuItem properties;
        properties.id = "properties";
        properties.label = i18n::tr("Properties…");
        properties.keys = keys_for(Command::properties);
        properties.separator = true;
        properties.enabled = names.size() == 1 || (properties_window_ && !names.empty());
        properties.action = [this] { show_properties(); };
        items.push_back(std::move(properties));
        for (auto& item : action_items(action_context(folder))) {
            items.push_back(std::move(item));
        }
        return items;
    }
    const auto add = [&items](std::string id, std::string label, std::string keys,
                              std::function<void()> action, bool separator = false,
                              bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = label == "Open" ? i18n::trc("file-action", "Open") : i18n::tr(label);
        item.keys = std::move(keys);
        item.action = std::move(action);
        item.separator = separator;
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    add("open", "Open", "Enter", [this] {
        ui::KeyEvent enter;
        enter.keysym = XKB_KEY_Return;
        view_key(enter, Clock::now());
    });
    if (cursor != nullptr && choices_ && open_with_) {
        MenuItem with;
        with.id = "open_with";
        with.label = i18n::tr("Open With");
        with.submenu = true;
        with.empty_text = i18n::tr("Looking for applications…");
        items.push_back(std::move(with));
        // What it would open: the selected entries of the cursor's kind (files, or folders), else
        // the cursor's; their types as known.
        std::vector<fs::Entry> files;
        for (const fs::Entry& entry : folder->entries()) {
            if (entry.is_directory() == cursor->is_directory() &&
                target_names.contains(entry.name)) {
                fs::Entry typed = entry;
                typed.mime = folder->mime_of(entry);
                files.push_back(std::move(typed));
            }
        }
        fs::Entry typed = *cursor;
        typed.mime = folder->mime_of(*cursor);
        menu_loaders_.emplace_back([this, directory = folder->path(), typed = std::move(typed),
                                    files = std::move(files)]() mutable {
            ++menu_loads_;
            choices_(directory, std::move(typed),
                     [this, alive = alive_, generation = menu_generation_, directory,
                      files = std::move(files)](const std::vector<AppChoice>& apps) {
                         if (!*alive) {
                             return;
                         }
                         --menu_loads_;
                         if (generation != menu_generation_) {
                             return;  // another menu now
                         }
                         std::vector<MenuItem> children;
                         for (const AppChoice& app : apps) {
                             MenuItem item;
                             item.id = app.id;
                             item.label = app.is_default
                                              ? app.name + std::string{i18n::tr(" (default)")}
                                              : app.name;
                             item.action = [this, directory, files, id = app.id] {
                                 open_with_(directory, files, id);
                             };
                             children.push_back(std::move(item));
                         }
                         if (children.empty()) {
                             MenuItem none;  // a dimmed line says so
                             none.label = i18n::tr("No applications");
                             none.enabled = false;
                             children.push_back(std::move(none));
                         }
                         menu_.set_submenu("open_with", std::move(children));
                     });
        });
    }
    const std::size_t count = names.size();
    std::vector<std::filesystem::path> folders;
    for (const fs::Entry& entry : folder->entries()) {
        if (entry.is_directory() && target_names.contains(entry.name)) {
            folders.push_back(folder->path() / entry.name);
        }
    }
    if (!folders.empty() && folders.size() == count) {
        // Behind the tab shown, in their order: each right after the one before.
        add("open_tab", count == 1 ? "Open in New Tab" : "Open in New Tabs", "",
            [this, folders] { open_tabs(folders); });
    }
    if (cursor != nullptr && cursor->is_directory() && terminal_) {
        add("open_terminal", "Open in Terminal", keys_for(Command::open_terminal),
            [this] { open_terminal(); });
    }
    if (!folders.empty() && folders.size() == count) {
        add("add_bookmark", "Add to Sidebar", count == 1 ? keys_for(Command::add_bookmark) : "",
            [this, folders] { add_to_sidebar(folders); });
    }
    if (photos_available() && !folder->is_archive() && folders.size() == 1 && count == 1)
        add("photos_add_folder", "Add to Photo Library…", "",
            [this, path = folders.front()] { open_photo_library(path); });
    if (const RunningTargets running = running_targets(*folder); !running.files.empty()) {
        const bool stop = running.all_run;
        add("running", stop ? "Stop Running as Program" : "Allow Running as Program", "",
            [this, stop] { set_running(!stop); });
    }
    add("compress_zip", "Compress to ZIP…", "", [this] { compress(false); }, true);
    add("compress_tar", "Compress to tar.gz…", "", [this] { compress(true); });
    add("copy", "Copy", keys_for(Command::copy), [this] { take_selection(false); }, true);
    add("cut", "Cut", keys_for(Command::cut), [this] { take_selection(true); });
    add(
        "paste", "Paste", keys_for(Command::paste), [this] { paste(); }, false,
        !clipboard_.names.empty() || (system_.types && !system_.types().empty()));
    add(
        "rename", count > 1 ? "Batch Rename…" : "Rename…", keys_for(Command::rename),
        [this] { start_rename(); }, true, count == 1 || (!picker_mode_ && count > 1));
    add("trash", "Move to Trash", keys_for(Command::trash), [this] { trash(); });
    if (!folder->is_archive() && !picker_mode_)
        add("move_into_folder", std::string{i18n::mark("Move into New Folder…")}, "",
            [this] { move_into_new_folder(); });
    if (!folder->is_archive() && !picker_mode_)
        for (auto& item : link_menu(false))
            items.push_back(std::move(item));
    if (count == 1 && cursor && cursor->is_link && !folder->is_archive())
        add("reveal_link_target", std::string{i18n::mark("Go to Link Target")}, "",
            [this] { reveal_link_target(); });
    add("erase", "Delete Permanently…", keys_for(Command::erase), [this] { erase(); });
    add(
        "properties", "Properties…", keys_for(Command::properties), [this] { show_properties(); },
        true, count == 1 || (properties_window_ && count > 1));
    if (!picker_mode_ && count == 1 && cursor && cursor->is_directory() && !folder->is_archive())
        add("folder_insights", "Insights…", keys_for(Command::folder_insights),
            [this, path = folder->path_of(*cursor)] { show_insights(path); });
    for (auto& item : action_items(action_context(folder))) {
        items.push_back(std::move(item));
    }
    items.push_back(sort_menu());
    return items;
}

std::vector<MenuItem> Browser::folder_menu() {
    std::vector<MenuItem> items;
    if (photos_available()) {
        const auto folder = current_folder();
        if (folder && !folder->is_trash() && !folder->is_archive()) {
            MenuItem item;
            item.id = "photos_add_folder";
            item.label = i18n::tr("Add to Photo Library…");
            item.action = [this, path = folder->path()] { open_photo_library(path); };
            items.push_back(std::move(item));
        }
    }
    MenuItem make_folder;
    make_folder.id = "new_folder";
    make_folder.label = i18n::tr("New Folder");
    make_folder.keys = keys_for(Command::new_folder);
    make_folder.action = [this] { new_folder(); };
    items.push_back(std::move(make_folder));

    // "New File": an empty one at once, the templates when they have been looked for.
    const auto empty_file = [this] {
        MenuItem item;
        item.id = "empty_file";
        item.label = i18n::tr("Empty File");
        item.action = [this] { new_file(std::string{i18n::tr("New File")}, {}); };
        return item;
    };
    MenuItem make_file;
    make_file.id = "new_file";
    make_file.label = i18n::tr("New File");
    make_file.submenu = true;
    make_file.children.push_back(empty_file());
    items.push_back(std::move(make_file));
    menu_loaders_.emplace_back([this, empty_file] {
        ++menu_loads_;
        pool_.submit(
            core::Priority::visible,
            [sources = place_sources_, fixed = templates_dir_](const core::CancelToken& /*token*/) {
                std::filesystem::path directory = fixed ? *fixed : fs::templates_dir(sources);
                std::vector<std::string> names =
                    directory.empty() ? std::vector<std::string>{} : fs::templates_in(directory);
                return std::pair{std::move(directory), std::move(names)};
            },
            [this, alive = alive_, generation = menu_generation_,
             empty_file](const std::pair<std::filesystem::path, std::vector<std::string>>& found) {
                if (!*alive) {
                    return;
                }
                --menu_loads_;
                if (generation != menu_generation_ || found.second.empty()) {
                    return;
                }
                std::vector<MenuItem> children{empty_file()};
                bool first = true;
                for (const std::string& name : found.second) {
                    MenuItem item;
                    item.id = "template:" + name;
                    item.label = name;
                    item.separator = std::exchange(first, false);
                    item.action = [this, name, from = found.first / name] { new_file(name, from); };
                    children.push_back(std::move(item));
                }
                menu_.set_submenu("new_file", std::move(children));
            });
    });

    MenuItem paste_here;
    paste_here.id = "paste";
    paste_here.label = i18n::tr("Paste");
    paste_here.keys = keys_for(Command::paste);
    paste_here.separator = true;
    paste_here.enabled = !clipboard_.names.empty() || (system_.types && !system_.types().empty());
    paste_here.action = [this] { paste(); };
    items.push_back(std::move(paste_here));

    if (const auto folder = current_folder();
        !picker_mode_ && folder && !folder->is_archive() && !folder->is_trash()) {
        MenuItem links;
        links.id = "paste_links";
        links.label = i18n::tr("Paste as Links");
        links.submenu = true;
        links.children = link_menu(true);
        links.enabled = !clipboard_.names.empty() || !clipboard_.paths.empty() ||
                        (system_.types && !system_.types().empty());
        items.push_back(std::move(links));
    }

    if (const std::shared_ptr<Folder> shown = current_folder()) {
        MenuItem bookmark;
        bookmark.id = "add_bookmark";
        bookmark.label = i18n::tr("Add to Sidebar");
        bookmark.keys = keys_for(Command::add_bookmark);
        bookmark.separator = true;
        bookmark.action = [this, path = shown->path()] { add_to_sidebar({path}); };
        items.push_back(std::move(bookmark));
        if (terminal_) {
            MenuItem terminal;
            terminal.id = "open_terminal";
            terminal.label = i18n::tr("Open in Terminal");
            terminal.keys = keys_for(Command::open_terminal);
            terminal.action = [this, path = shown->path()] { terminal_(path); };
            items.push_back(std::move(terminal));
        }
        MenuItem copy_path;
        copy_path.id = "copy_path";
        copy_path.label = std::string{i18n::tr("Copy Path")};
        copy_path.keys = keys_for(Command::copy_path);
        copy_path.action = [this] { this->copy_path(); };
        items.push_back(std::move(copy_path));
    }
    for (auto& item : action_items(action_context(current_folder(), true))) {
        items.push_back(std::move(item));
    }
    items.push_back(sort_menu());
    MenuItem properties;
    properties.id = "properties";
    properties.label = i18n::tr("Properties…");
    properties.keys = keys_for(Command::properties);
    properties.separator = true;
    properties.action = [this] { show_properties(); };
    items.push_back(std::move(properties));
    const auto shown = current_folder();
    if (!picker_mode_ && shown && !shown->is_archive() && !shown->is_trash()) {
        MenuItem insights;
        insights.id = "folder_insights";
        insights.label = i18n::tr("Insights…");
        insights.keys = keys_for(Command::folder_insights);
        insights.action = [this, path = shown->path()] { show_insights(path); };
        items.push_back(std::move(insights));
    }
    return items;
}

void Browser::show_insights(std::filesystem::path path, std::optional<InsightsView::View> initial) {
    if (showing_photos()) {
        open_photo_menu(insights_bounds_.x, insights_bounds_.bottom(), false);
        return;
    }
    const auto folder = current_folder();
    if (!folder || folder->is_archive() || folder->is_trash() || picker_mode_)
        return;
    if (path.empty()) {
        path = folder->path();
        if (const auto* selected = folder->single_selection(); selected && selected->is_directory())
            path = folder->path_of(*selected);
    }
    if (!pane().insights.is_open())
        open_tab(path, true);
    menu_.close();
    palette_.close();
    quick_look_.close();
    shortcuts_.close();
    info_pane_.clear_text_focus();
    info_pane_.cancel_measurement();
    filter_focused_ = false;
    pane().insights.set_filter(pane().valid_filter);
    pane().insights.open(
        std::move(path), pane().show_hidden,
        [this](const std::filesystem::path& target, bool directory) {
            set_name_filter({});
            if (std::ranges::any_of(target, [](const std::filesystem::path& part) {
                    const auto& name = part.native();
                    return name != "." && name != ".." && !name.empty() && name.front() == '.';
                }))
                set_show_hidden(true, false);
            if (directory)
                open(target);
            else
                show_found(target.parent_path(), target.filename().string(), FoundAction::show);
        },
        initial);
}

void Browser::show_properties() {
    if (showing_photos()) {
        open_photo_properties();
        return;
    }
    const auto folder = current_folder();
    if (!folder || properties_.saving())
        return;
    const auto names = targets(*folder);
    if ((!properties_window_ && names.size() > 1) || (names.empty() && folder->is_trash()))
        return;
    if (properties_window_ && names.size() > 1) {
        menu_.close();
        palette_.close();
        quick_look_.close();
        info_pane_.clear_text_focus();
        filter_focused_ = false;
        for (const auto& name : names) {
            const auto entry = std::ranges::find(folder->entries(), name, &fs::Entry::name);
            if (entry != folder->entries().end())
                open_properties(folder->path_of(*entry),
                                !folder->is_trash() && !folder->is_archive());
        }
        return;
    }
    auto path = folder->path();
    if (!names.empty()) {
        const auto it = std::ranges::find(folder->entries(), names.front(), &fs::Entry::name);
        if (it == folder->entries().end())
            return;
        path = folder->path_of(*it);
    }
    menu_.close();
    palette_.close();
    quick_look_.close();
    info_pane_.clear_text_focus();
    filter_focused_ = false;
    open_properties(path, !folder->is_trash() && !folder->is_archive());
}

void Browser::open_properties(const std::filesystem::path& path, bool editable, bool drive,
                              std::string label) {
    if (properties_window_) {
        properties_window_(path, editable, drive, label);
        return;
    }
    properties_.open(
        path, [this, path] { properties_changed(path); }, editable, drive, std::move(label));
}

void Browser::properties_changed(const std::filesystem::path& path) {
    info_pane_.refresh();
    set_places(places_);
    const auto refresh = [&path](Folder& owner) {
        if (owner.path() == path.parent_path())
            owner.refresh_entry(path.filename().string());
    };
    for (const auto& owner : areas_) {
        if (!owner)
            continue;
        for (const auto& tab : owner->tabs) {
            if (tab->grid.folder())
                refresh(*tab->grid.folder());
            tab->details.refresh_entry(path);
            for (std::size_t i = 0; i < tab->columns.column_count(); ++i)
                refresh(tab->columns.column(i).folder());
        }
    }
    redraw_();
}

namespace {

// Default groups are kept together here, after all applicable commands (including panes and
// actions) are known. Explicit user layouts still receive the complete available command list.
std::vector<MenuItem> default_context_menu(std::vector<MenuItem> items, bool background,
                                           bool trash) {
    std::vector<core::MenuNode> layout;
    const auto group = [&](std::initializer_list<std::string_view> ids) {
        layout.push_back({.separator = true});
        for (const auto id : ids) {
            // Respect action placement: do not pull a user-configured submenu child to the root.
            if (std::ranges::find(items, id, &MenuItem::id) != items.end())
                layout.push_back({.id = std::string{id}});
        }
    };
    const auto extensions = [&] {
        layout.push_back({.separator = true, .remaining = true});
        layout.push_back({.id = "actions"});
    };
    if (trash) {
        group({"restore"});
        group({"action:copy_name"});
        group({"erase", "empty_trash"});
    } else if (background) {
        // These commands copy the same current-folder path. Keep one default entry, with its
        // shortcut, while retaining both IDs for user layouts and the command palette.
        const auto path = std::ranges::find(items, "copy_path", &MenuItem::id);
        const auto location = std::ranges::find(items, "action:copy_location", &MenuItem::id);
        if (path != items.end() && location != items.end()) {
            if (location->keys.empty())
                location->keys = path->keys;
            items.erase(path);
        }
        group({"new_folder", "new_file"});
        group({"paste", "paste_links", "action:copy_location"});
        group({"open_terminal", "add_bookmark", "photos_add_folder"});
        extensions();
    } else {
        group({"open", "open_with", "open_tab", "open_terminal", "reveal_link_target"});
        group({"cut", "copy", "paste", "rename", "create_link", "create_relative_link",
               "create_hard_link", "move_into_folder"});
        group({"action:copy_name", "action:copy_location"});
        group({"action:extract_here", "action:extract_to", "compress_zip", "compress_tar"});
        group({"add_bookmark", "photos_add_folder", "running"});
        extensions();
        group({"trash", "erase"});
    }
    group({"sort", "panes"});
    group({"properties", "folder_insights"});
    return arrange_menu(items, layout);
}

}  // namespace

void Browser::open_menu(float x, float y, bool keyboard) {
    if (showing_photos()) {
        menu_.open(x, y, photo_library_->context_menu_items(), keyboard);
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (sidebar_.focused()) {
        sidebar_.set_focused(false);
    }
    ++menu_generation_;
    menu_loaders_.clear();
    std::vector<MenuItem> items;
    if (folder->is_archive()) {
        if (!targets(*folder).empty()) {
            MenuItem copy;
            copy.id = "copy";
            copy.label = std::string{i18n::tr("Copy to Extract")};
            copy.keys = keys_for(Command::copy);
            copy.action = [this] { take_selection(false); };
            MenuItem preview;
            preview.id = "quick_look";
            preview.label = std::string{i18n::tr("Quick Look")};
            preview.keys = "Space";
            preview.action = [this, folder] { quick_look_.open(folder, Clock::now()); };
            items.push_back(std::move(preview));
            copy.separator = true;
            items.push_back(std::move(copy));
            menu_.open(x, y, std::move(items), keyboard);
        }
        return;
    }
    if (!targets(*folder).empty()) {
        items = entry_menu(folder);
    } else if (!folder->is_trash()) {
        items = folder_menu();
    } else {
        MenuItem empty;  // nothing is made in the trash
        empty.id = "empty_trash";
        empty.label = i18n::tr("Empty Trash…");
        empty.enabled = !folder->entries().empty();
        empty.action = [this] { empty_trash(); };
        items.push_back(std::move(empty));
    }
    const auto& layout = folder->is_trash()          ? menu_layout_.trash
                         : !targets(*folder).empty() ? menu_layout_.entry
                                                     : menu_layout_.background;
    auto pane_commands = pane_items();
    if (!pane_commands.empty()) {
        MenuItem submenu;
        submenu.id = "panes";
        submenu.label = std::string{i18n::tr("Panes")};
        submenu.submenu = true;
        submenu.children = std::move(pane_commands);
        items.push_back(std::move(submenu));
    }
    if (!layout && !menu_layout_.items)
        items =
            default_context_menu(std::move(items), targets(*folder).empty(), folder->is_trash());
    items = configured_menu(std::move(items), layout);
    if (items.empty()) {
        menu_loaders_.clear();
        return;
    }
    menu_.open(x, y, std::move(items), keyboard);
    // Submenus found later, once the menu they fill is open (the answer may come at once).
    for (const std::function<void()>& load : std::exchange(menu_loaders_, {})) {
        load();
    }
}

void Browser::open_menu_from_keyboard() {
    if (sidebar_.focused()) {
        if (const auto* place = sidebar_.cursor_place()) {
            const auto [x, y] = sidebar_.cursor_anchor();
            place_menu(*place, x, y, true);
        }
        return;
    }
    std::optional<std::pair<float, float>> anchor;
    if (pane().insights.is_open()) {
        const auto rect = pane().insights.selected_bounds();
        open_menu(rect.x + 8, rect.bottom(), true);
        return;
    }
    switch (pane().mode) {
        case Mode::columns:
            anchor = pane().columns.cursor_anchor();
            break;
        case Mode::grid:
            anchor = pane().grid.cursor_anchor();
            break;
        case Mode::details:
            anchor = pane().details.cursor_anchor();
            break;
    }
    // No entry shown under the cursor: near the view's top left.
    const auto [x, y] = anchor.value_or(std::pair{
        area().view_area.x + (area().view_area.width / 4.0F), area().view_area.y + 40.0F});
    open_menu(x, y, true);
}

void Browser::rename(const std::shared_ptr<Folder>& folder, const std::string& from,
                     const std::string& to) {
    if (folder->is_collection())
        return;
    if (batch_rename_preparing_ || batch_rename_running_) {
        return;
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path(), from, to](const core::CancelToken& /*token*/) {
            return fs::rename_entry(directory, from, to);
        },
        [this, alive = alive_, folder, from, to](const fs::Renamed& renamed) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (renamed.error) {
                show_message(renamed.problem, true);
                editor_.finished(false);
                return;
            }
            // The cursor follows the new name by itself (the watcher reports a rename), unless it
            // still waits for the old one: a new folder renamed before the watcher saw it.
            // A hidden name would leave the listing: the cursor keeps it in sight.
            if (folder->cursor_pending() || (!folder->shows_hidden() && to.starts_with('.'))) {
                folder->expect_name(to);
            }
            record({.kind = FileOperation::Kind::rename,
                    .items = {{.from = folder->path() / from, .to = folder->path() / to}},
                    .modes = {}},
                   Purpose::user, false);
            if (!renamed.problem.empty()) {
                show_message(renamed.problem, true);
            } else if (renamed.sidecars > 0) {
                show_message(
                    i18n::trn("Renamed {1} with {0} darktable sidecar",
                              "Renamed {1} with {0} darktable sidecars", renamed.sidecars, to),
                    false);
            }
            editor_.finished(true);
            if (searching_results())
                search_results_->refresh();
        });
}

void Browser::open(const std::filesystem::path& directory) {
    pane().photos = false;
    pane().photo_records = false;
    clear_type_jump();
    if (pane().insights.is_open()) {
        ++path_navigation_generation_;
        quick_look_.close();
        menu_.close();
        if (directory == fs::trash_path())
            open_tab(directory, true);
        else
            pane().insights.navigate(directory);
        redraw_();
        return;
    }
    ++path_navigation_generation_;
    info_pane_.clear_text_focus();
    cancel_operation_motion();
    remember_folder(pane());
    pane().restore_mode_on_arrival = false;
    pane().columns.open(directory);
    if (pane().mode == Mode::grid) {
        pane().columns.suspend();
        pane().grid.show(pane().columns.focused_folder());
    } else if (pane().mode == Mode::details) {
        pane().columns.suspend();
        pane().details.show(pane().columns.focused_folder());
    }
    restore_folder(directory, true);
    note_location();
}

void Browser::reveal(const std::filesystem::path& root, const std::filesystem::path& target) {
    remember_folder(pane());
    pane().retained_folder_mode.reset();
    pane().settings_folder.clear();  // unfolding columns does not change the folder left
    set_mode(Mode::columns);
    pane().restore_mode_on_arrival = true;
    pane().columns.reveal(root, target);
    note_location();
}

State::FolderSettings Browser::folder_settings(const Pane& each) {
    return {.mode = each.retained_folder_mode.value_or(static_cast<State::Mode>(each.mode)),
            .sort_key = each.details.order().key,
            .sort_descending = each.details.order().descending,
            .details_widths = each.details.widths(),
            .details_columns = each.details.columns(),
            .list_size = each.details.list_size(),
            .grid_icon = static_cast<std::int32_t>(each.grid.icon_size())};
}

void Browser::remember_folder(Pane& each) {
    if (each.photos || each.settings_folder.empty())
        return;
    const auto settings = folder_settings(each);
    // Two visible panes may show the same folder with different settings. Only actual
    // changes in this pane update the shared record; drawing cannot alternate it every frame.
    if (each.remembered_settings != settings) {
        folder_settings_.remember(each.settings_folder, settings);
        each.remembered_settings = settings;
    }
}

void Browser::restore_folder(const std::filesystem::path& path, bool opened) {
    const bool restore_retained = opened && pane().retained_folder_mode.has_value();
    if (!restore_retained && pane().settings_folder == path)
        return;
    const auto normalized = path.lexically_normal();
    if (!restore_retained && pane().settings_folder == normalized)
        return;
    remember_folder(pane());
    pane().settings_folder = normalized;
    pane().remembered_settings.reset();
    pane().retained_folder_mode.reset();
    if (const auto* saved = folder_settings_.find(normalized)) {
        const auto settings = *saved;
        set_sort(settings.sort_key, settings.sort_descending);
        set_list_size(settings.list_size);
        set_grid_size(grid_index(settings.grid_icon));
        pane().details.set_widths(settings.details_widths);
        pane().details.set_columns(settings.details_columns);
        if (opened || pane().mode != Mode::columns)
            set_mode(static_cast<Mode>(settings.mode));
        else
            pane().retained_folder_mode = settings.mode;
    }
    const auto settings = folder_settings(pane());
    folder_settings_.remember(normalized, settings, true);
    pane().remembered_settings = settings;
}

void Browser::show_items(const std::vector<std::filesystem::path>& items, bool details) {
    ++show_items_pending_;
    const auto addressed = active_area_;
    pool_.submit(
        core::Priority::visible,
        [items](const core::CancelToken& cancel) {
            std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> groups;
            for (const auto& item : items) {
                if (cancel.cancelled())
                    break;
                auto normal = item.lexically_normal();
                if (!normal.is_absolute())
                    continue;
                if (!normal.has_filename() && normal.has_relative_path())
                    normal = normal.parent_path();
                std::error_code error;
                if (!std::filesystem::exists(std::filesystem::symlink_status(normal, error)) ||
                    error)
                    continue;
                const bool root = !normal.has_relative_path();
                const auto folder = root ? normal : normal.parent_path();
                if (!std::filesystem::is_directory(folder, error))
                    continue;
                auto group = std::ranges::find(groups, folder,
                                               [](const auto& entry) { return entry.first; });
                if (group == groups.end())
                    group = groups.insert(groups.end(), {folder, {}});
                if (!root && std::ranges::find(group->second, normal.filename().string()) ==
                                 group->second.end()) {
                    group->second.push_back(normal.filename().string());
                }
            }
            return groups;
        },
        [this, alive = alive_, addressed, details](const auto& groups) {
            if (!*alive)
                return;
            --show_items_pending_;
            if (!areas_[addressed])
                return;
            AreaContext context{context_area_, addressed};
            const auto first = area().active;
            for (std::size_t i = 0; i < groups.size(); ++i) {
                const auto& [folder, names] = groups[i];
                if (i == 0)
                    open(folder);
                else
                    open_tab(folder, true);
                set_name_filter({});
                filter_focused_ = false;
                sidebar_.set_focused(false);
                // Reveal all requested hidden items, not just the first name's filter exception.
                if (std::ranges::any_of(names,
                                        [](const auto& name) { return name.starts_with('.'); })) {
                    set_show_hidden(true, false);
                }
                if (const auto shown = current_folder(); shown && !names.empty())
                    shown->expect_names(names);
            }
            if (groups.size() > 1)
                select_tab(first);
            if (details && properties_window_) {
                for (const auto& [directory, names] : groups) {
                    if (names.empty())
                        open_properties(directory);
                    else
                        for (const auto& name : names)
                            open_properties(directory / name);
                }
            } else if (details && !groups.empty()) {
                set_info_visible(true);
                info_pane_.set_expanded(true);
            }
            redraw_();
        });
}

void Browser::note_location() {
    if (pane().photos)
        return;
    if (pane().insights.is_open())
        return;
    if (pane().mode == Mode::columns && pane().columns.revealing()) {
        return;  // noted once the columns have unfolded
    }
    const std::shared_ptr<Folder> shown =
        pane().mode == Mode::details ? pane().details.root_folder() : current_folder();
    if (!shown) {
        return;
    }
    if (!shown->is_trash()) {
        restore_folder(shown->path(), std::exchange(pane().restore_mode_on_arrival, false));
        if (context_area_ == active_area_)
            last_folder_ = shown->path();
    } else {
        remember_folder(pane());
        pane().settings_folder.clear();
        pane().retained_folder_mode.reset();
    }
    remember_folder(pane());
    const fs::Entry* const cursor = shown->cursor_entry();
    if (!pane().history.empty() && pane().history[pane().here].folder == shown->path()) {
        if (cursor != nullptr) {
            pane().history[pane().here].cursor = cursor->name;
        }
        return;
    }
    if (!pane().history.empty()) {
        pane().history.erase(pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here) + 1,
                             pane().history.end());
    }
    pane().history.push_back({.folder = shown->path(), .cursor = cursor ? cursor->name : ""});
    if (pane().history.size() > kHistorySize) {
        pane().history.erase(pane().history.begin());
    }
    pane().here = pane().history.size() - 1;
}

bool Browser::can_travel(int direction) const {
    if (showing_photos())
        return direction < 0 && !photo_library_->at_all_photos();
    if (pane().insights.is_open())
        return direction < 0 && pane().insights.can_back();
    return direction < 0 ? !pane().history.empty() && pane().here > 0
                         : pane().here + 1 < pane().history.size();
}

void Browser::navigation_pointer(const ui::PointerEvent& event) {
    const int direction = area().back.contains(event.x, event.y)      ? -1
                          : area().forward.contains(event.x, event.y) ? 1
                                                                      : 0;
    if (event.kind == ui::PointerEvent::Kind::press && event.button == ui::PointerButton::left) {
        area().navigation_pressed = direction && can_travel(direction) ? direction : 0;
        redraw_();
    } else if (event.kind == ui::PointerEvent::Kind::release &&
               event.button == ui::PointerButton::left) {
        const int pressed = std::exchange(area().navigation_pressed, 0);
        if (pressed && pressed == direction && can_travel(direction))
            travel(direction);
        redraw_();
    } else if (event.kind == ui::PointerEvent::Kind::leave) {
        area().navigation_pressed = 0;
        redraw_();
    }
}

void Browser::back() { travel(-1); }

void Browser::forward() { travel(1); }

void Browser::travel(int direction) {
    if (showing_photos()) {
        if (direction < 0)
            photo_library_->show_all_photos();
        return;
    }
    if (pane().insights.is_open()) {
        if (direction < 0) {
            if (pane().insights.can_back())
                pane().insights.back();
        }
        return;
    }
    note_location();  // the cursor where it is now
    quick_look_.close();
    const auto ahead = [&] {
        return direction < 0 ? pane().here > 0 : pane().here + 1 < pane().history.size();
    };
    std::vector<std::string> gone;
    const auto report_gone = [&] {
        if (!gone.empty()) {
            show_message(gone.size() == 1 ? i18n::trf("{} no longer exists", gone.front())
                                          : i18n::trn("{} folder no longer exists",
                                                      "{} folders no longer exist", gone.size()),
                         true);
        }
    };
    bool moved = false;
    while (ahead()) {
        area().motion_back = true;
        pane().here = direction < 0 ? pane().here - 1 : pane().here + 1;
        const Step step = pane().history[pane().here];  // go_to() notes the cursor it finds there
        go_to(step.folder, step.cursor);
        moved = true;
        const std::shared_ptr<Folder> shown = current_folder();
        if (!shown) {
            return;
        }
        // Opening a folder that is not there fails at once (it cannot be watched).
        const std::error_code& error = shown->model().error();
        if (error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
            if (!step.cursor.empty() &&
                (shown->cursor_entry() == nullptr || shown->cursor_entry()->name != step.cursor)) {
                shown->select_name(step.cursor);
            }
            pane().history[pane().here].cursor = step.cursor;
            report_gone();
            return;
        }
        gone.push_back(step.folder.filename().string());
        pane().history.erase(pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here));
        if (direction > 0) {
            --pane().here;  // back on the step we left: the next one ahead took its place
        }
    }
    if (moved) {
        const Step step = pane().history[pane().here];
        go_to(step.folder, step.cursor);  // nothing there: back where it started
    }
    report_gone();
}

void Browser::set_list_size(std::size_t index) {
    pane().columns.set_list_size(index);
    pane().details.set_list_size(index);
}

void Browser::set_grid_size(std::size_t index) { pane().grid.set_size_index(index); }

std::size_t Browser::grid_index(std::int32_t pixels) {
    const auto* found = std::ranges::find(kGridIconSizes, pixels);
    return found == kGridIconSizes.end() ? kUsualGridSize
                                         : static_cast<std::size_t>(found - kGridIconSizes.begin());
}

void Browser::set_sort(fs::SortKey key, bool descending) {
    pane().details.set_sort(key, descending);
    pane().columns.set_order(pane().details.order());
    pane().grid.set_order(pane().details.order());
    if (showing_photos())
        photo_library_->set_sort(pane().details.order());
}

MenuItem Browser::details_columns_menu() {
    MenuItem menu;
    menu.id = "details_columns";
    menu.label = std::string{i18n::tr("Show columns")};
    menu.submenu = true;
    menu.separator = true;
    for (const auto& column : kDetailsColumns) {
        if (showing_photos() && column.field != fs::SortKey::name &&
            column.field != fs::SortKey::size && column.field != fs::SortKey::modified &&
            column.field != fs::SortKey::location)
            continue;
        MenuItem item;
        item.id = "column:" + std::string{column.id};
        item.label = i18n::tr(column.title);
        item.checkable = true;
        item.checked = pane().details.column_visible(column.field);
        item.enabled = column.field != DetailsView::Field::name;
        item.action = [this, field = column.field] {
            pane().details.set_column_visible(field, !pane().details.column_visible(field));
        };
        menu.children.push_back(std::move(item));
    }
    MenuItem reset;
    reset.id = "columns_reset";
    reset.label = std::string{i18n::tr("Reset columns")};
    reset.separator = true;
    reset.action = [this] {
        pane().details.set_columns(kDefaultDetailsColumns);
        pane().details.set_widths(kDetailsWidths);
    };
    menu.children.push_back(std::move(reset));
    return menu;
}

std::vector<MenuItem> Browser::insights_menu() {
    using View = InsightsView::View;
    const auto& insights = pane().insights;
    const bool open = insights.is_open();
    auto current = insights.view();
    if (current == View::other_types || current == View::matches)
        current = View::types;
    else if (current == View::age_matches)
        current = View::age;
    else if (current == View::code_matches)
        current = View::languages;
    std::vector<MenuItem> items;
    const auto view = [&](std::string id, std::string_view label, View target) {
        MenuItem item;
        item.id = std::move(id);
        item.label = label;
        item.radio = true;
        item.checked = open && current == target;
        item.action = [this, target] {
            if (pane().insights.is_open())
                pane().insights.set_view(target);
            else
                show_insights({}, target);
        };
        items.push_back(std::move(item));
    };
    view("insights_types", i18n::tr("Types"), View::types);
    view("insights_space", i18n::tr("Map"), View::space);
    view("insights_iris", i18n::tr("Iris"), View::iris);
    view("insights_largest", i18n::tr("Largest"), View::largest);
    view("insights_age", i18n::tr("Age"), View::age);
    view("insights_languages", i18n::tr("Languages"), View::languages);
    if (open) {
        if (current != View::languages && current != View::largest) {
            MenuItem measure;
            measure.id = "insights_measure";
            measure.label = i18n::tr("Size by:");
            measure.submenu = true;
            measure.separator = true;
            for (const bool bytes : {true, false}) {
                MenuItem item;
                item.id = bytes ? "insights_bytes" : "insights_count";
                item.label = bytes ? i18n::tr("Size") : i18n::tr("Count");
                item.radio = true;
                item.checked = insights.size_measure() == bytes;
                item.action = [this, bytes] { pane().insights.set_size_measure(bytes); };
                measure.children.push_back(std::move(item));
            }
            items.push_back(std::move(measure));
        }
        MenuItem recursive;
        recursive.id = "insights_recursive";
        recursive.label = i18n::tr("Subfolders");
        recursive.separator = true;
        recursive.checkable = true;
        recursive.checked = insights.recursive();
        recursive.action = [this] { pane().insights.set_recursive(!pane().insights.recursive()); };
        items.push_back(std::move(recursive));
        MenuItem hidden;
        hidden.id = "insights_hidden";
        hidden.label = i18n::tr("Hidden files");
        hidden.checkable = true;
        hidden.checked = insights.hidden();
        hidden.action = [this] { pane().insights.set_hidden(!pane().insights.hidden()); };
        items.push_back(std::move(hidden));
    }
    return items;
}

MenuItem Browser::sort_menu() {
    const fs::SortOrder& now = sort_order();
    MenuItem sort;
    sort.id = "sort";
    sort.label =
        i18n::tr(has_custom_folder_view() ? i18n::mark("Sort By · Custom folder view") : "Sort By");
    sort.submenu = true;
    sort.separator = true;
    const auto choice = [](std::string id, std::string label, bool checked,
                           std::function<void()> action, bool separator = false) {
        MenuItem item;
        item.id = std::move(id);
        item.label = i18n::tr(label);
        item.radio = true;
        item.checked = checked;
        item.separator = separator;
        item.action = std::move(action);
        return item;
    };

    if (pane().insights.is_open()) {
        auto& insights = pane().insights;
        sort.label = i18n::tr("Sort By");
        for (const auto& column : insights.sort_columns())
            sort.children.push_back(
                choice("sort:" + std::string{column.id}, std::string{column.label},
                       insights.sort_key() == column.key, [this, key = column.key] {
                           pane().insights.set_sort(key, pane().insights.sort_descending());
                       }));
        for (const bool descending : {false, true})
            sort.children.push_back(choice(
                descending ? "sort:descending" : "sort:ascending",
                descending ? "Descending" : "Ascending", insights.sort_descending() == descending,
                [this, descending] {
                    pane().insights.set_sort(
                        pane().insights.sort_key().value_or(InsightsView::SortKey::name),
                        descending);
                },
                !descending));
        return sort;
    }
    for (const auto& column : kDetailsColumns) {
        if (showing_photos() && column.field != fs::SortKey::name &&
            column.field != fs::SortKey::size && column.field != fs::SortKey::modified &&
            column.field != fs::SortKey::location)
            continue;
        sort.children.push_back(choice(
            "sort:" + std::string{column.id}, std::string{column.title}, now.key == column.field,
            [this, key = column.field] { set_sort(key, sort_order().descending); }));
    }
    sort.children.push_back(choice(
        "sort:ascending", "Ascending", !now.descending,
        [this] { set_sort(sort_order().key, false); }, true));
    sort.children.push_back(choice("sort:descending", "Descending", now.descending,
                                   [this] { set_sort(sort_order().key, true); }));
    if (const auto folder = current_folder(); folder && !folder->is_trash() && !picker_mode_) {
        MenuItem reset;
        reset.id = "reset_folder_view";
        reset.label = i18n::tr("Use Default View for This Folder");
        reset.keys = keys_for(Command::reset_folder_view);
        reset.separator = true;
        reset.enabled = has_custom_folder_view();
        reset.action = [this] { reset_folder_view(); };
        sort.children.push_back(std::move(reset));
    }
    return sort;
}

void Browser::set_entry_filter(fs::Listing::Predicate filter) {
    picker_filter_ = std::move(filter);
    apply_filters();
}

MenuItem Browser::name_filter_menu() {
    using Pattern = fs::NamePattern;
    MenuItem menu;
    menu.id = "name_filter_mode";
    menu.label = i18n::tr("Name matching");
    menu.submenu = true;
    auto spec = Pattern::parse(name_filter());
    for (const auto& [mode, label] : {std::pair{Pattern::Mode::automatic, i18n::mark("Automatic")},
                                      {Pattern::Mode::text, i18n::mark("Plain text")},
                                      {Pattern::Mode::glob, i18n::mark("Glob")},
                                      {Pattern::Mode::regex, i18n::mark("Regular expression")}}) {
        MenuItem item;
        item.id = "filter_mode_" + std::to_string(static_cast<int>(mode));
        item.label = i18n::tr(label);
        item.radio = true;
        item.checked = spec.mode == mode;
        item.action = [this, spec, mode]() mutable {
            spec.mode = mode;
            set_name_filter(Pattern::encode(spec));
            filter_focused_ = true;
        };
        menu.children.push_back(std::move(item));
    }
    MenuItem sensitive;
    sensitive.id = "filter_case";
    sensitive.label = i18n::tr("Case sensitive");
    sensitive.separator = true;
    sensitive.checkable = true;
    sensitive.checked = spec.case_sensitive;
    sensitive.action = [this, spec]() mutable {
        spec.case_sensitive = !spec.case_sensitive;
        set_name_filter(Pattern::encode(spec));
        filter_focused_ = true;
    };
    menu.children.push_back(std::move(sensitive));
    return menu;
}

void Browser::set_name_filter(std::string text) {
    pane().filter_field.set_text(std::move(text));
    apply_filters();
}

void Browser::apply_filters() {
    if (showing_photos()) {
        pane().filter_error.clear();
        pane().valid_filter = pane().filter_field.text();
        photo_library_->set_query(pane().valid_filter);
        return;
    }
    fs::NamePattern pattern{pane().filter_field.text()};
    if (pattern.error().empty() && !pane().filter_error.empty() &&
        status_bar_.message() == pane().filter_error)
        show_message({}, false);
    pane().filter_error = pattern.error();
    if (!pane().filter_error.empty()) {
        show_message(pane().filter_error, true);
        pattern = fs::NamePattern{pane().valid_filter};
    } else {
        pane().valid_filter = pane().filter_field.text();
    }
    pane().insights.set_filter(pane().valid_filter);
    pane().entry_filter = [filter = picker_filter_, pattern](const fs::Entry& entry) {
        return (!filter || filter(entry)) && pattern.matches(entry.name);
    };
    pane().columns.set_entry_filter(pane().entry_filter);
    pane().grid.set_entry_filter(pane().entry_filter);
    pane().details.set_entry_filter(pane().entry_filter);
}

void Browser::set_show_hidden(bool show, bool announce) {
    pane().insights.set_hidden(show);
    pane().show_hidden = show;
    pane().columns.set_show_hidden(show);
    pane().grid.set_show_hidden(show);
    pane().details.set_show_hidden(show);
    if (announce)
        show_message(
            std::string{show ? i18n::tr("Hidden files shown") : i18n::tr("Hidden files hidden")},
            false);
    redraw_();
}

void Browser::set_info_visible(bool visible) {
    info_visible_ = visible;
    if (visible)
        info_auto_hide_ = false;
    if (!visible) {
        info_pane_.cancel_measurement();
        info_pane_.clear_text_focus();
    }
    for (const std::unique_ptr<Pane>& each : area().tabs) {
        each->grid.set_info_visible(visible);
        each->details.set_info_visible(visible);
    }
}

void Browser::set_info_auto_hide(bool enabled) {
    info_auto_hide_ = enabled;
    if (enabled)
        info_visible_ = true;
    redraw_();
}

bool Browser::info_shown() const {
    const auto folder = current_folder();
    // Metadata-only content is useful too. Loading or failed decodes never change visibility.
    return info_visible_ && (!info_auto_hide_ || (folder && folder->cursor_entry()));
}

void Browser::set_sidebar_visible(bool visible) {
    if (visible == sidebar_visible_) {
        return;
    }
    sidebar_visible_ = visible;
    if (!visible) {
        sidebar_.confirm_rename();
        sidebar_.set_focused(false);
        sidebar_.not_drawn();
        if (pointer_target_ == Target::sidebar) {
            send_pointer(Target::sidebar, {.kind = ui::PointerEvent::Kind::leave}, Clock::now());
            pointer_target_ = Target::view;
            button_held_ = false;
        }
    }
    redraw_();
}

// The remembered mode is the browser's, by value.
static_assert(static_cast<int>(State::Mode::columns) == static_cast<int>(Browser::Mode::columns) &&
              static_cast<int>(State::Mode::grid) == static_cast<int>(Browser::Mode::grid) &&
              static_cast<int>(State::Mode::details) == static_cast<int>(Browser::Mode::details));

State Browser::state() {
    const auto context = context_area_;
    context_area_ = active_area_;
    note_location();
    context_area_ = context;
    State state;
    state.folders = folder_settings_;
    state.last_folder = last_folder_;
    state.mode = static_cast<State::Mode>(pane().mode);
    state.show_hidden = pane().show_hidden;
    state.show_extensions = show_extensions_;
    state.show_thumbnails = show_thumbnails_;
    state.gallery = gallery_;
    state.sidebar_visible = sidebar_visible_;
    state.sidebar_width = sidebar_.width();
    state.sidebar_size = sidebar_.list_size();
    state.sidebar_order = sidebar_.order();
    state.sidebar_hidden = sidebar_.hidden();
    state.sidebar_collapsed = sidebar_.collapsed();
    state.info_visible = info_visible_;
    state.info_auto_hide = info_auto_hide_;
    state.info_width = info_pane_.width();
    state.sidebar_preview_ratio = sidebar_preview_ratio_;
    state.info_expanded = info_pane_.expanded();
    state.sort_key = pane().details.order().key;
    state.sort_descending = pane().details.order().descending;
    state.details_widths = pane().details.widths();
    state.details_columns = pane().details.columns();
    state.list_size = pane().details.list_size();
    state.grid_icon = static_cast<std::int32_t>(pane().grid.icon_size());
    state.palette_recent = palette_recent_;
    state.saved_searches = saved_searches_;
    state.photo_library = photo_library_ ? photo_library_->state() : photo_state_;
    state.split_visible = split_visible_;
    state.panes_initialized = areas_[1] != nullptr;
    state.stacked = stacked_;
    state.split_ratio = split_ratio_;
    state.active_pane = active_area_;
    for (std::size_t side = 0; side < areas_.size(); ++side) {
        if (!areas_[side])
            continue;
        const BrowsingArea& owner = *areas_[side];
        if (owner.tabs.size() <= 1 && !areas_[1] && !owner.tabs.front()->photos)
            continue;
        for (std::size_t i = 0; i < owner.tabs.size() && i < State::kMaxTabs; ++i) {
            const Pane& each = *owner.tabs[i];
            const std::shared_ptr<Folder> folder =
                each.mode == Mode::details ? each.details.root_folder() : folder_of(each);
            if (!folder || folder->is_trash()) {
                continue;
            }
            State::Tab tab = tab_of(each, side);
            if (i == owner.active) {
                auto index = static_cast<std::size_t>(
                    std::ranges::count(state.tabs, side, &State::Tab::pane));
                if (side == 0)
                    state.shown_tab = index;
                else
                    state.other_shown_tab = index;
            }
            state.tabs.push_back(std::move(tab));
        }
    }
    return state;
}

State::Tab Browser::tab_of(const Pane& each, std::size_t side) {
    const auto folder = each.mode == Mode::details ? each.details.root_folder() : folder_of(each);
    State::Tab tab{.folder = folder->path(),
                   .mode = static_cast<State::Mode>(each.mode),
                   .root = {},
                   .cursor = {},
                   .list_size = {},
                   .grid_icon = {}};
    if (each.mode == Mode::columns && each.columns.column_count() > 0) {
        tab.root = each.columns.column(0).folder().path();
    }
    if (const fs::Entry* entry = folder->cursor_entry()) {
        tab.cursor = entry->name;
    }
    tab.list_size = each.details.list_size();
    tab.grid_icon = static_cast<std::int32_t>(each.grid.icon_size());
    tab.pane = side;
    tab.sort_key = each.details.order().key;
    tab.details_columns = each.details.columns();
    tab.details_widths = each.details.widths();
    tab.sort_descending = each.details.order().descending;
    tab.show_hidden = each.show_hidden;
    tab.filter = each.valid_filter;
    tab.photos = each.photos;
    if (each.photos) {
        tab.folder.clear();
        tab.root.clear();
        if (const auto* cursor = folder->cursor_entry())
            tab.cursor = cursor->key();
        if (each.photo_records) {
            tab.mode = static_cast<State::Mode>(each.photo_view);
            tab.details_columns = each.photo_columns;
            tab.cursor.clear();
        }
    }
    return tab;
}

void Browser::restore_tab(const State::Tab& tab, std::size_t window_list, std::size_t window_grid) {
    if (tab.photos) {
        if (!photos_available()) {
            open(home_directory());
            show_message(std::string{i18n::tr("Photo Library is unavailable; opened Home instead")},
                         false);
            return;
        }
        prepare_photo_library();
        pane().photos = true;
        pane().settings_folder.clear();
        pane().mode = Mode::columns;
        pane().columns.show(photo_library_->folder());
        const auto supported = [](fs::SortKey key) {
            return key == fs::SortKey::name || key == fs::SortKey::size ||
                   key == fs::SortKey::modified || key == fs::SortKey::location;
        };
        auto columns = tab.details_columns.value_or(std::vector<fs::SortKey>{
            fs::SortKey::name, fs::SortKey::size, fs::SortKey::modified, fs::SortKey::location});
        std::erase_if(columns, [&](fs::SortKey key) { return !supported(key); });
        pane().details.set_columns(std::move(columns));
        if (tab.details_widths)
            pane().details.set_widths(*tab.details_widths);
        set_mode(static_cast<Mode>(tab.mode));
        pane().photo_view = pane().mode;
        set_list_size(tab.list_size.value_or(window_list));
        set_grid_size(tab.grid_icon ? grid_index(*tab.grid_icon) : window_grid);
        const auto sort = tab.sort_key.value_or(fs::SortKey::name);
        set_sort(supported(sort) ? sort : fs::SortKey::name, tab.sort_descending);
        set_name_filter(tab.filter);
        photo_library_->restore_cursor(tab.cursor);
        photo_library_->open();
        sync_photo_view();
        return;
    }
    pane().mode = static_cast<Mode>(tab.mode);
    set_list_size(tab.list_size.value_or(window_list));
    set_grid_size(tab.grid_icon ? grid_index(*tab.grid_icon) : window_grid);
    pane().retained_folder_mode.reset();
    pane().restore_mode_on_arrival = false;
    pane().settings_folder = tab.folder.lexically_normal();
    pane().remembered_settings.reset();
    if (const auto* settings = folder_settings_.find(pane().settings_folder)) {
        if (pane().mode == Mode::columns)
            pane().retained_folder_mode = settings->mode;
        set_sort(settings->sort_key, settings->sort_descending);
        pane().details.set_widths(settings->details_widths);
        pane().details.set_columns(settings->details_columns);
    }
    const std::filesystem::path relative = tab.folder.lexically_relative(tab.root);
    if (pane().mode == Mode::columns && tab.root.is_absolute() && !relative.empty() &&
        *relative.begin() != "..") {
        // The columns unfold as they were, down to the cursor in the folder shown.
        pane().columns.reveal(tab.root, tab.cursor.empty() ? tab.folder : tab.folder / tab.cursor);
    } else {
        open(tab.folder);
        if (const std::shared_ptr<Folder> opened = current_folder();
            opened && !tab.cursor.empty()) {
            opened->select_name(tab.cursor);
        }
    }
    set_sort(tab.sort_key.value_or(pane().details.order().key),
             tab.sort_key ? tab.sort_descending : pane().details.order().descending);
    set_show_hidden(tab.show_hidden.value_or(pane().show_hidden), false);
    if (tab.details_columns)
        pane().details.set_columns(*tab.details_columns);
    if (tab.details_widths)
        pane().details.set_widths(*tab.details_widths);
    set_name_filter(tab.filter);
    note_location();
}

bool Browser::restore_tabs(const std::vector<State::Tab>& tabs, std::size_t shown) {
    // Tabs written before they had a zoom of their own take the window's (apply()).
    const std::size_t window_list = pane().details.list_size();
    const std::size_t window_grid = pane().grid.size_index();
    std::size_t restored = 0;
    std::size_t to_show = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const State::Tab& tab = tabs[i];
        if (tab.photos ? !photos_available() : !tab.folder.is_absolute()) {
            continue;  // malformed paths are ignored; disconnected folders stay recoverable
        }
        if (restored > 0) {
            add_pane(restored);
        }
        area().active = restored;
        restore_tab(tab, window_list, window_grid);
        if (i == shown) {
            to_show = restored;
        }
        ++restored;
    }
    area().active = std::min(to_show, area().tabs.size() - 1);
    redraw_();
    return restored > 0;
}

void Browser::apply(const State& state) {
    folder_settings_ = state.folders;
    last_folder_ = state.last_folder;
    for (const auto& each : area().tabs) {
        each->settings_folder.clear();
        each->remembered_settings.reset();
        each->retained_folder_mode.reset();
        each->restore_mode_on_arrival = false;
    }
    show_extensions_ = state.show_extensions;
    show_thumbnails_ = state.show_thumbnails;
    gallery_ = state.gallery;
    pane().show_hidden = state.show_hidden;
    palette_recent_ = state.palette_recent;
    saved_searches_ = state.saved_searches;
    photo_state_ = state.photo_library;
    show_places();
    for (const std::unique_ptr<Pane>& each : area().tabs) {
        each->show_hidden = state.show_hidden;
        each->columns.set_show_hidden(state.show_hidden);
        each->grid.set_show_hidden(state.show_hidden);
        each->details.set_show_hidden(state.show_hidden);
        each->details.set_widths(state.details_widths);
        each->details.set_columns(state.details_columns);
    }
    set_sidebar_visible(state.sidebar_visible);
    sidebar_.set_width(state.sidebar_width);
    sidebar_.set_list_size(state.sidebar_size);
    sidebar_.set_organization(state.sidebar_order, state.sidebar_hidden, state.sidebar_collapsed);
    set_info_visible(state.info_visible);
    info_auto_hide_ = state.info_auto_hide;
    info_pane_.set_width(state.info_width);
    sidebar_preview_ratio_ = state.sidebar_preview_ratio;
    info_pane_.set_expanded(state.info_expanded);
    set_sort(state.sort_key, state.sort_descending);
    set_list_size(state.list_size);
    set_grid_size(grid_index(state.grid_icon));
    const auto mode = static_cast<Mode>(state.mode);
    if (current_folder()) {
        set_mode(mode);
    } else {
        pane().mode = mode;  // open() shows the folder in it
    }
    redraw_();
}

void Browser::set_mode(Mode mode) {
    clear_type_jump();
    if (pane().insights.is_open())
        open_tab(pane().insights.current_path(), true);
    pane().retained_folder_mode.reset();
    if (mode == pane().mode) {
        return;
    }
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    // The view that leaves forgets the pointer; the one that comes finds it where it is.
    const auto now = Clock::now();
    const ui::PointerEvent leave{.kind = ui::PointerEvent::Kind::leave};
    const ui::PointerEvent motion{
        .kind = ui::PointerEvent::Kind::motion, .x = pointer_x_, .y = pointer_y_};
    if (pointer_target_ == Target::view) {
        view_pointer(leave, now);
    }
    if (pane().mode == Mode::columns) {
        pane().columns.suspend();
    }
    pane().mode = mode;
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.show(std::move(folder));
            break;
        case Mode::grid:
            pane().grid.show(std::move(folder));
            break;
        case Mode::details:
            pane().details.show(std::move(folder));
            break;
    }
    if (pointer_inside_) {
        dispatch_pointer(motion, now);
    }
    redraw_();
}

void Browser::draw_toolbar(ui::Renderer& renderer, const ui::Rect& bounds,
                           const theme::Palette& palette, ui::IconSet& icons) {
    toolbar_bounds_ = bounds;
    sidebar_toggle_bounds_ = {};
    filter_bounds_ = clear_filter_bounds_ = new_folder_bounds_ = {};
    view_menu_bounds_ = insights_bounds_ = sort_bounds_ = {};
    if (bounds.height < 40 || bounds.width < 180) {
        return;
    }
    renderer.fill_background(bounds, palette.surface_container);
    const float separator_height = 1.0F / renderer.scale();
    renderer.fill_rect(
        {bounds.x, bounds.bottom() - separator_height, bounds.width, separator_height},
        palette.outline_variant);
    constexpr float button_width = 30;
    constexpr float gap = 4;
    sidebar_toggle_bounds_ = {bounds.x + 6, bounds.y + 6, button_width, 32};
    const float field_x = sidebar_toggle_bounds_.right() + 8;
    float field_right = bounds.right() - 6;
    // Reserve the view selector first; lower-priority actions collapse in narrow windows.
    const auto reserve = [&](ui::Rect& rect) {
        if (field_right - field_x < button_width + gap + 64)
            return;
        rect = {field_right - button_width, bounds.y + 6, button_width, 32};
        field_right = rect.x - gap;
    };
    reserve(view_menu_bounds_);
    const auto folder = current_folder();
    if ((panes_enabled_ || showing_photos()) && !picker_mode_ && folder && !folder->is_archive() &&
        !folder->is_trash())
        reserve(insights_bounds_);
    reserve(sort_bounds_);
    reserve(new_folder_bounds_);
    filter_bounds_ = {field_x, bounds.y + 6, std::max(1.0F, field_right - field_x), 32};
    if (!name_filter().empty() && filter_bounds_.width >= button_width + gap + 48) {
        filter_bounds_.width -= button_width + gap;
        clear_filter_bounds_ = {filter_bounds_.right() + gap, bounds.y + 6, button_width, 32};
    }
    const auto text_in = [&](std::string_view text, const ui::Rect& rect, ui::Color color) {
        const auto& line =
            renderer.fonts().layout(text, 13, renderer.scale(), std::max(1.0F, rect.width));
        const float height = (line.ascent + line.descent) / renderer.scale();
        renderer.draw_layout(line, rect.x, rect.y + (rect.height - height) / 2, color);
    };
    pane().filter_field.draw(
        renderer, filter_bounds_, 14,
        {.background = palette.surface,
         .border = !pane().filter_error.empty()                         ? palette.error
                   : (filter_focused_ && context_area_ == active_area_) ? palette.primary
                                                                        : palette.outline_variant,
         .text = palette.on_surface,
         .selection = palette.primary_container,
         .caret = (filter_focused_ && context_area_ == active_area_)
                      ? palette.primary
                      : palette.primary.with_alpha(0)});
    if (name_filter().empty() && !filter_focused_) {
        text_in(
            showing_photos() ? std::string{i18n::tr("Search photos…")} : i18n::tr("Filter names…"),
            {filter_bounds_.x + 6, filter_bounds_.y, filter_bounds_.width - 12,
             filter_bounds_.height},
            palette.on_surface_variant);
    }
    const auto button = [&](const ui::Rect& rect, std::string_view icon, bool active = false) {
        if (rect.empty())
            return;
        const bool hovered = pointer_inside_ && rect.contains(pointer_x_, pointer_y_);
        renderer.draw_box(rect, {.fill = active    ? palette.primary_container
                                         : hovered ? palette.surface_container_highest
                                                   : palette.surface_container_high,
                                 .radius = {}});
        icons.draw(renderer, icon, rect.x + (rect.width - 18) / 2, rect.y + 7, 18,
                   active ? palette.on_primary_container : palette.on_surface);
    };
    button(sidebar_toggle_bounds_, "layout-columns", sidebar_visible_);
    const auto view_icon = pane().mode == Mode::columns ? "layout-columns"
                           : pane().mode == Mode::grid  ? "layout-grid"
                                                        : "list-details";
    button(view_menu_bounds_, view_icon);
    // A small disclosure mark distinguishes the view menu from a mode toggle.
    if (!view_menu_bounds_.empty())
        icons.draw(renderer, "chevron-down", view_menu_bounds_.right() - 9,
                   view_menu_bounds_.bottom() - 10, 8, palette.on_surface);
    button(insights_bounds_, showing_photos() ? "photo" : "database",
           showing_photos() || pane().insights.is_open());
    if (!insights_bounds_.empty())
        icons.draw(renderer, "chevron-down", insights_bounds_.right() - 9,
                   insights_bounds_.bottom() - 10, 8,
                   pane().insights.is_open() ? palette.on_primary_container : palette.on_surface);
    button(sort_bounds_, "adjustments-horizontal");
    button(clear_filter_bounds_, "x");
    button(new_folder_bounds_, "folder");
    if (!new_folder_bounds_.empty()) {
        const auto& box = new_folder_bounds_;
        renderer.fill_rect({box.right() - 11, box.y + 16, 8, 1.5F}, palette.on_surface);
        renderer.fill_rect({box.right() - 7.75F, box.y + 12.75F, 1.5F, 8}, palette.on_surface);
    }
}

std::pair<ui::Rect, std::string> Browser::control_hint_at(float x, float y) const {
    if (has_overlay() || settings_.is_open() || batch_rename_.is_open())
        return {};
    if (showing_photos()) {
        if (new_folder_bounds_.contains(x, y))
            return {new_folder_bounds_, "Add to Photo Library…"};
        if (insights_bounds_.contains(x, y))
            return {insights_bounds_, "Photo Library"};
    }
    for (const auto& [rect, label] :
         {std::pair{sidebar_toggle_bounds_, i18n::mark("Places sidebar")},
          {new_folder_bounds_, i18n::mark("New Folder")},
          {sort_bounds_, i18n::mark("Sort By")},
          {view_menu_bounds_, i18n::mark("View")},
          {insights_bounds_, i18n::mark("Insights")},
          {clear_filter_bounds_, i18n::mark("Clear")}}) {
        if (rect.contains(x, y))
            return {rect, std::string{i18n::tr(label)}};
    }
    for (const auto& [rect, label] :
         {std::pair{status_bar_.settings_bounds(), i18n::mark("Settings…")},
          {status_bar_.help_bounds(), i18n::mark("Keyboard shortcuts")},
          {status_bar_.cancel_bounds(), i18n::mark("Stop the Transfer")}}) {
        if (rect.contains(x, y))
            return {rect, std::string{i18n::tr(label)}};
    }
    return {};
}

void Browser::clear_control_hint() {
    loop_.remove(control_hint_timer_);
    control_hint_timer_ = 0;
    control_hint_ready_ = false;
    if (!control_hint_text_.empty()) {
        control_hint_text_.clear();
        redraw_();
    }
}

void Browser::draw_control_hint(ui::Renderer& renderer, const ui::Rect& bounds,
                                const theme::Palette& palette) {
    if (!control_hint_ready_ || control_hint_text_.empty() || !pointer_inside_)
        return;
    const auto [anchor, text] = control_hint_at(pointer_x_, pointer_y_);
    if (text != control_hint_text_ || anchor.x != control_hint_anchor_.x ||
        anchor.y != control_hint_anchor_.y) {
        clear_control_hint();
        return;
    }
    const auto& line =
        renderer.fonts().layout(text, 12, renderer.scale(), std::max(1.0F, bounds.width - 32));
    const float width = line.width / renderer.scale() + 16;
    const float height = (line.ascent + line.descent) / renderer.scale() + 12;
    const float below = anchor.bottom() + 6;
    const ui::Rect box{
        std::clamp(anchor.x + (anchor.width - width) / 2, bounds.x + 4,
                   std::max(bounds.x + 4, bounds.right() - width - 4)),
        std::clamp(below + height <= bounds.bottom() - 4 ? below : anchor.y - height - 6,
                   bounds.y + 4, std::max(bounds.y + 4, bounds.bottom() - height - 4)),
        width, height};
    renderer.draw_box(box, {.fill = palette.surface_container_highest,
                            .radius = {},
                            .border_width = 1,
                            .border = palette.outline_variant});
    renderer.draw_layout(line, box.x + 8, box.y + 6, palette.on_surface);
}

void Browser::toolbar_pointer(const ui::PointerEvent& event) {
    if (event.kind != ui::PointerEvent::Kind::press && filter_focused_ &&
        pane().filter_field.pointer(event)) {
        redraw_();
        return;
    }

    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    if (sidebar_toggle_bounds_.contains(event.x, event.y)) {
        set_sidebar_visible(!sidebar_visible_);
    } else if (view_menu_bounds_.contains(event.x, event.y)) {
        open_view_menu(view_menu_bounds_.x, view_menu_bounds_.bottom());
    } else if (sort_bounds_.contains(event.x, event.y)) {
        filter_focused_ = false;
        menu_.open(sort_bounds_.x, sort_bounds_.bottom(), sort_menu().children, false);
    } else if (insights_bounds_.contains(event.x, event.y)) {
        if (showing_photos())
            open_photo_menu(insights_bounds_.x, insights_bounds_.bottom(), false);
        else {
            filter_focused_ = false;
            menu_.open(insights_bounds_.x, insights_bounds_.bottom(), insights_menu(), false);
        }
    } else if (filter_bounds_.contains(event.x, event.y)) {
        filter_focused_ = true;
        pane().filter_field.pointer(event);
    } else if (clear_filter_bounds_.contains(event.x, event.y)) {
        set_name_filter({});
        filter_focused_ = true;
    } else if (new_folder_bounds_.contains(event.x, event.y)) {
        new_folder();
    }
    redraw_();
}

void Browser::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons, Clock::time_point now) {
    if (password_.is_open() || errors_.is_open() || conflict_.is_open() || confirm_.is_open() ||
        mount_password_.is_open() || location_.is_open() || batch_rename_.is_open() ||
        settings_.is_open() || properties_.is_open() || palette_.is_open() || menu_.is_open() ||
        shortcuts_.is_open() || quick_look_.is_open() || editor_.active())
        info_pane_.clear_text_focus();
    begin_motion_frame(renderer, bounds, now);
    context_area_ = active_area_;
    note_location();
    if (current_folder())
        sidebar_.set_show_hidden(shows_hidden());
    const float sidebar_width = sidebar_visible_ ? sidebar_.width_in(bounds.width) : 0.0F;
    sidebar_drawn_ = sidebar_width > 0.0F;
    info_pane_.forget_layout();
    shared_info_bounds_ = {};
    sidebar_preview_divider_ = {};
    sidebar_bounds_ = {bounds.x, bounds.y, sidebar_width, bounds.height};
    preview_below_places_ = panes_enabled_ && preview_in_sidebar_ && sidebar_drawn_ &&
                            info_shown() && !pane().insights.is_open() && bounds.height >= 240;
    if (!sidebar_drawn_ && sidebar_.focused())
        sidebar_.set_focused(false);
    settle_found(now);
    const auto folder = current_folder();
    if (showing_photos() && info_shown()) {
        auto photo = photo_library_->properties(true);
        info_pane_.set_photo_properties(
            photo.entry.identity.empty() ? std::nullopt : std::optional{std::move(photo)});
    } else
        info_pane_.set_photo_properties(std::nullopt);
    status_bar_.set_collection_status(showing_photos() ? photo_library_->status() : std::string{});
    sidebar_.set_current_uri(showing_photos() ? photo_library_->navigation_uri() : "");
    if (sidebar_drawn_) {
        float places_height = bounds.height;
        if (preview_below_places_) {
            places_height =
                std::clamp(bounds.height * sidebar_preview_ratio_, 100.0F, bounds.height - 120);
            sidebar_preview_divider_ = {bounds.x, bounds.y + places_height, sidebar_width, 6};
            shared_info_bounds_ = {bounds.x, sidebar_preview_divider_.bottom(), sidebar_width,
                                   bounds.height - places_height - 6};
        }
        sidebar_.draw(renderer, {bounds.x, bounds.y, sidebar_width, places_height}, palette, icons,
                      folder ? folder->path() : std::filesystem::path{}, now);
    } else
        sidebar_.not_drawn();
    ui::Rect content{bounds.x + sidebar_width, bounds.y, bounds.width - sidebar_width,
                     bounds.height};
    const float bar = std::min(status_bar_.height(renderer), std::max(0.0F, content.height));
    if (folder)
        status_bar_.draw(renderer, {content.x, content.bottom() - bar, content.width, bar}, palette,
                         icons, *folder, panes_enabled_);
    content.height -= bar;
    const float toolbar_height = std::min(44.0F, std::max(0.0F, content.height - kTitleHeight));
    draw_toolbar(renderer, {content.x, content.y, content.width, toolbar_height}, palette, icons);
    content.y += toolbar_height;
    content.height -= toolbar_height;
    if (split_visible_ && info_shown() && !preview_below_places_ && !pane().insights.is_open()) {
        const float width =
            info_pane_.width_in(content.width - (stacked_ ? 0.0F : InfoPane::kMinContent));
        if (width > 0) {
            shared_info_bounds_ = {content.right() - width, content.y, width, content.height};
            content.width -= width;
        }
    }
    split_bounds_ = content;
    divider_ = {};
    for (auto& owner : areas_)
        if (owner)
            owner->bounds = {};
    if (editor_.active() && !editor_.busy()) {
        const Folder& edited = *editor_.folder();
        if (!edited.cursor_pending() &&
            std::ranges::find(edited.entries(), editor_.slot().name, &fs::Entry::name) ==
                edited.entries().end())
            editor_.cancel();
    }
    editor_.slot().rect.reset();
    if (split_visible_) {
        const float length = std::max(0.0F, (stacked_ ? content.height : content.width) - 6);
        const float minimum = std::min(stacked_ ? 160.0F : 220.0F, length / 2);
        const float first = std::clamp(length * split_ratio_, minimum, length - minimum);
        std::array<ui::Rect, 2> rectangles;
        if (stacked_) {
            rectangles = {{{content.x, content.y, content.width, first},
                           {content.x, content.y + first + 6, content.width, length - first}}};
            divider_ = {content.x, content.y + first, content.width, 6};
        } else {
            rectangles = {{{content.x, content.y, first, content.height},
                           {content.x + first + 6, content.y, length - first, content.height}}};
            divider_ = {content.x + first, content.y, 6, content.height};
        }
        // Paint the inactive pane first; shared editors and preview belong to the active one.
        for (const auto index : {1 - active_area_, active_area_}) {
            context_area_ = index;
            auto colors = palette;
            if (index != active_area_) {
                colors.primary = palette.on_surface_variant;
                colors.primary_container = palette.surface_container_highest;
                colors.on_primary_container = palette.on_surface;
            }
            draw_area(renderer, rectangles[index], colors, icons, now);
        }
        renderer.fill_background(divider_, palette.outline_variant);
    } else
        draw_area(renderer, content, palette, icons, now);
    context_area_ = active_area_;
    if (folder && shared_info_bounds_.width > 0)
        info_pane_.draw(renderer, shared_info_bounds_, palette, icons, *folder, now);
    info_pane_.end_frame();
    if (!sidebar_preview_divider_.empty()) {
        renderer.fill_background(sidebar_preview_divider_, palette.outline_variant);
        const float edge = 1.0F / renderer.scale();
        renderer.fill_rect({shared_info_bounds_.right() - edge, shared_info_bounds_.y, edge,
                            shared_info_bounds_.height},
                           palette.outline_variant);
    }
    draw_drive_tooltip(renderer, palette, bounds);
    draw_control_hint(renderer, bounds, palette);
    draw_band(renderer, palette);
    editor_.draw(renderer, palette);
    if (showing_photos() && photo_library_->has_dialog())
        photo_library_->draw_dialogs(renderer, bounds, palette);
    if (searching_results())
        search_results_->draw(renderer, bounds, palette);
    if (quick_look_.is_open())
        quick_look_.follow(searching_results() && search_preview_ ? search_preview_
                                                                  : current_folder());
    const auto overlay_motion = begin_overlay_motion(renderer, bounds, palette, icons, now);
    quick_look_.draw(renderer, bounds, palette, icons, now);
    shortcuts_.draw(renderer, bounds, palette);
    menu_.draw(renderer, bounds, palette);
    palette_.draw(renderer, bounds, palette);
    end_overlay_motion(renderer, overlay_motion, bounds, palette);
    properties_.draw(renderer, bounds, palette, icons, thumbnails_.get());
    settings_.draw(renderer, bounds, palette, icons);
    conflict_.draw(renderer, bounds, palette);
    confirm_.draw(renderer, bounds, palette);
    errors_.draw(renderer, bounds, palette);
    password_.draw(renderer, bounds, palette);
    batch_rename_.draw(renderer, bounds, palette);
    location_.draw(renderer, bounds, palette);
    mount_password_.draw(renderer, bounds, palette);
    if (thumbnails_)
        thumbnails_->end_frame();
}

void Browser::draw_area(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, ui::IconSet& icons, Clock::time_point now) {
    note_location();  // includes asynchronous reveals in either visible pane
    layout_motion_area(bounds);
    area().bounds = bounds;
    ui::Rect view = bounds;
    const auto folder = current_folder();
    renderer.push_clip(bounds);
    area().back = area().forward = area().selection_count = {};
    if ((split_visible_ || area().tabs.size() > 1 || pane().insights.is_open()) &&
        view.height > TabBar::kHeight + kTitleHeight) {
        area().tab_bar.draw(renderer, {view.x, view.y, view.width, TabBar::kHeight}, palette, icons,
                            tab_titles(), area().active);
        view.y += TabBar::kHeight;
        view.height -= TabBar::kHeight;
    } else {
        area().tab_bar.not_drawn();
    }
    const float bar = split_visible_ ? std::min(status_bar_.height(renderer),
                                                std::max(0.0F, view.height - kTitleHeight))
                                     : 0;
    if (folder) {
        const auto location = pane().insights.is_open() && pane().insights.scope_folder()
                                  ? pane().insights.scope_folder()
                              : pane().mode == Mode::details ? pane().details.root_folder()
                                                             : folder;
        float left = view.x;
        if (view.width >= 140 && view.height >= kTitleHeight) {
            area().back = {view.x + 6, view.y + (kTitleHeight - 26) / 2, 26, 26};
            area().forward = {area().back.right() + 2, area().back.y, 26, 26};
            left = area().forward.right() + 2;
            for (const int direction : {-1, 1}) {
                const auto& rect = direction < 0 ? area().back : area().forward;
                if (area().navigation_pressed == direction && can_travel(direction))
                    renderer.draw_box(rect, {.fill = palette.surface_container_highest,
                                             .radius = ui::Corners::all(13)});
                icons.draw(renderer, direction < 0 ? "chevron-left" : "chevron-right", rect.x + 5,
                           rect.y + 5, 16,
                           can_travel(direction) ? palette.on_surface : palette.outline_variant);
            }
            const float edge = 1.0F / renderer.scale();
            renderer.fill_rect({view.x, view.y + kTitleHeight - edge, left - view.x, edge},
                               palette.outline_variant);
        }
        float right = view.right();
        if (folder->selected_count() > 0 && view.height >= kTitleHeight) {
            const auto label = i18n::trf("{} selected", folder->selected_count());
            const float available = std::max(0.0F, right - left);
            const float width = std::min(
                available * 0.5F,
                renderer.fonts().layout(label, 12.0F, renderer.scale()).width / renderer.scale() +
                    20.0F);
            if (width >= 24.0F) {
                area().selection_count = {right - width, view.y, width, kTitleHeight};
                const auto badge = area().selection_count;
                renderer.fill_background(badge, palette.surface_container);
                renderer.draw_box(
                    {badge.x + 4, badge.y + 7, badge.width - 8, badge.height - 14},
                    {.fill = palette.secondary_container, .radius = ui::Corners::all(4)});
                renderer.draw_text(label, badge.x + 8, badge.y + (badge.height - 16) / 2, 12,
                                   palette.on_secondary_container, badge.width - 16);
                right = badge.x;
            }
        }
        area().breadcrumb.draw(renderer, {left, view.y, std::max(0.0F, right - left), kTitleHeight},
                               palette, icons, *location);
        if (split_visible_) {
            const ui::Rect footer{view.x, view.bottom() - bar, view.width, bar};
            renderer.fill_background(footer, palette.surface_container);
            const auto& line =
                renderer.fonts().layout(status_of(*folder), status_bar_.text_size(),
                                        renderer.scale(), std::max(1.0F, footer.width - 16));
            const float height = (line.ascent + line.descent) / renderer.scale();
            renderer.draw_layout(line, footer.x + 8, footer.y + (footer.height - height) / 2,
                                 palette.on_surface_variant);
        }
    }
    area().view_area = {view.x, view.y + kTitleHeight, view.width,
                        std::max(0.0F, view.height - kTitleHeight - bar)};
    pane().columns.set_info_visible(!split_visible_ && info_shown() && !preview_below_places_);
    pane().grid.set_info_visible(!split_visible_ && info_shown() && !preview_below_places_);
    if (pane().details.info_visible() !=
        (!split_visible_ && info_shown() && !preview_below_places_))
        pane().details.set_info_visible(!split_visible_ && info_shown() && !preview_below_places_);
    draw_view_with_motion(renderer, palette, icons, now);
    if (keyboard_focus_visible_ && context_area_ == active_area_ && !sidebar_.focused() &&
        !filter_focused_ && !area().breadcrumb.editing() && !sidebar_.renaming() &&
        !has_overlay() && !settings_.is_open() && !batch_rename_.is_open()) {
        const ui::Rect focus_bounds = area().view_area;
        const float edge = std::min({2.0F, focus_bounds.width, focus_bounds.height});
        renderer.fill_rect({focus_bounds.x, focus_bounds.y, focus_bounds.width, edge},
                           palette.primary);
        renderer.fill_rect({focus_bounds.x, focus_bounds.bottom() - edge, focus_bounds.width, edge},
                           palette.primary);
        renderer.fill_rect({focus_bounds.x, focus_bounds.y, edge, focus_bounds.height},
                           palette.primary);
        renderer.fill_rect({focus_bounds.right() - edge, focus_bounds.y, edge, focus_bounds.height},
                           palette.primary);
    }
    if (split_visible_ && context_area_ == active_area_)
        renderer.draw_box(bounds, {.fill = palette.surface.with_alpha(0),
                                   .radius = {},
                                   .border_width = 1.0F / renderer.scale(),
                                   .border = palette.primary.with_alpha(0.65F)});
    renderer.pop_clip();
}

void Browser::draw_view(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, ui::IconSet& icons, Clock::time_point now) {
    if (pane().insights.is_open()) {
        pane().insights.set_iris_motion(motion_.config().enabled && !motion_.reduced()
                                            ? 0.28F * motion_.config().duration_scale
                                            : 0);
        pane().insights.draw(renderer, bounds, palette, icons, *mime_, now);
        return;
    }
    pane().columns.set_show_thumbnails(show_thumbnails_);
    pane().grid.set_show_thumbnails(show_thumbnails_);
    pane().grid.set_gallery(gallery_);
    pane().details.set_show_thumbnails(show_thumbnails_);
    pane().columns.set_show_extensions(show_extensions_);
    pane().grid.set_show_extensions(show_extensions_);
    pane().details.set_show_extensions(show_extensions_);
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.draw(renderer, bounds, palette, icons, now);
            break;
        case Mode::grid:
            pane().grid.draw(renderer, bounds, palette, icons, now);
            break;
        case Mode::details:
            pane().details.draw(renderer, bounds, palette, icons, now);
            break;
    }
}

bool Browser::animating() const {
    if (motion_.active() || settings_.animating())
        return true;
    for (std::size_t i = 0; i < areas_.size(); ++i) {
        if (!areas_[i] || (!split_visible_ && i != active_area_))
            continue;
        const auto& tab = *areas_[i]->tabs[areas_[i]->active];
        if (tab.insights.animating() || (tab.mode == Mode::columns && tab.columns.animating()) ||
            (tab.mode == Mode::grid && tab.grid.animating()) ||
            (tab.mode == Mode::details && tab.details.animating()))
            return true;
    }
    return info_pane_.animating() || sidebar_.animating() || quick_look_.animating(Clock::now());
}

bool Browser::loading() const {
    if (show_items_pending_ > 0)
        return true;
    if ((showing_photos() && photo_library_->loading()) ||
        (searching_results() && search_results_->loading()) ||
        (search_preview_pending_ && search_preview_ && search_preview_->model().loading()))
        return true;
    if (finder_ && (finder_->scanning() || finder_->matching()))
        return true;
    if (operations_ > 0 || menu_loads_ > 0 || transfers_.pending() > 0 || quick_look_.loading() ||
        info_pane_.loading() || status_bar_.loading() || properties_.loading() ||
        properties_.saving() || settings_.busy() || (thumbnails_ && thumbnails_->loading()))
        return true;
    for (const auto& owner : areas_) {
        if (!owner)
            continue;
        for (const auto& tab : owner->tabs) {
            if (tab->insights.loading() || (tab->mode == Mode::columns && tab->columns.loading()) ||
                (tab->mode == Mode::grid && tab->grid.loading()) ||
                (tab->mode == Mode::details && tab->details.loading()))
                return true;
        }
    }
    return false;
}

void Browser::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.kind == ui::PointerEvent::Kind::press)
        clear_type_jump();
    if (event.kind == ui::PointerEvent::Kind::press)
        keyboard_focus_visible_ = false;
    if (event.kind == ui::PointerEvent::Kind::scroll) {
        cancel_operation_motion();
    }
    const bool button = event.kind == ui::PointerEvent::Kind::press ||
                        event.kind == ui::PointerEvent::Kind::release;
    if (button) {
        note_location();  // the cursor of the folder the click may leave
    }
    take_pointer(event, now);
    if (event.kind == ui::PointerEvent::Kind::motion && !button_held_) {
        const auto [anchor, text] = control_hint_at(event.x, event.y);
        if (text != control_hint_text_ || anchor.x != control_hint_anchor_.x ||
            anchor.y != control_hint_anchor_.y) {
            clear_control_hint();
            control_hint_text_ = text;
            control_hint_anchor_ = anchor;
            redraw_();
            if (!text.empty())
                control_hint_timer_ = loop_.add_timer(std::chrono::milliseconds{450},
                                                      std::chrono::milliseconds{0}, [this] {
                                                          control_hint_timer_ = 0;
                                                          control_hint_ready_ = true;
                                                          redraw_();
                                                      });
        }
    } else {
        clear_control_hint();
    }

    if (button) {
        note_location();
    }
    if (event.kind == ui::PointerEvent::Kind::leave) {
        // Someone else sets the shape now (the title bar, another window): send it again on
        // the way back.
        hover_cursor_ = ui::CursorShape::standard;
        shown_cursor_.reset();
        return;
    }
    hover_cursor_ = cursor_at(event.x, event.y);  // after the event: what it opened counts
    update_cursor();
}

std::function<void(ui::CursorShape)> Browser::resize_cursor() {
    return [this](ui::CursorShape shape) {
        resize_cursor_ = shape;
        update_cursor();
    };
}

void Browser::update_cursor() {
    const ui::CursorShape shape =
        resize_cursor_ != ui::CursorShape::standard ? resize_cursor_ : hover_cursor_;
    if (shape != shown_cursor_ && set_cursor_) {
        shown_cursor_ = shape;
        set_cursor_(shape);
    }
}

ui::CursorShape Browser::cursor_at(float x, float y) const {
    using Shape = ui::CursorShape;
    const auto hand = [](bool clickable) { return clickable ? Shape::pointer : Shape::standard; };
    // What takes the pointer first, as take_pointer() hands it out.
    if (password_.is_open())
        return hand(password_.clickable_at(x, y));
    if (errors_.is_open())
        return hand(errors_.clickable_at(x, y));
    if (conflict_.is_open()) {
        return hand(conflict_.clickable_at(x, y));
    }
    if (confirm_.is_open()) {
        return hand(confirm_.clickable_at(x, y));
    }
    if (mount_password_.is_open()) {
        return hand(mount_password_.clickable_at(x, y));
    }
    if (batch_rename_.is_open()) {
        return batch_rename_.cursor_at(x, y);
    }
    if (location_.is_open()) {
        return location_.field_at(x, y) ? Shape::text : hand(location_.clickable_at(x, y));
    }
    if (settings_.is_open())
        return settings_.cursor_at(x, y);
    if (properties_.is_open())
        return properties_.cursor_at(x, y);
    if (palette_.is_open()) {
        if (palette_.field_bounds().contains(x, y)) {
            return Shape::text;
        }
        return palette_.list_bounds().contains(x, y) ? Shape::pointer : Shape::standard;
    }
    if (menu_.is_open() || shortcuts_.is_open() || quick_look_.is_open()) {
        return Shape::standard;
    }
    if (showing_photos() && photo_library_->has_dialog())
        return photo_library_->cursor_at(x, y);
    if (searching_results())
        return search_results_->cursor_at(x, y);
    if (editor_.active() && editor_.field().bounds().contains(x, y)) {
        return Shape::text;
    }
    if (divider_.contains(x, y))
        return stacked_ ? Shape::ns_resize : Shape::ew_resize;
    if (sidebar_preview_divider_.contains(x, y))
        return Shape::ns_resize;
    if (!preview_below_places_ && shared_info_bounds_.width > 0 &&
        std::abs(x - shared_info_bounds_.x) <= InfoPane::kEdgeGrab && y >= shared_info_bounds_.y &&
        y < shared_info_bounds_.bottom())
        return Shape::ew_resize;
    if (filter_bounds_.contains(x, y))
        return Shape::text;
    if (clear_filter_bounds_.contains(x, y) || new_folder_bounds_.contains(x, y) ||
        view_menu_bounds_.contains(x, y) || sort_bounds_.contains(x, y) ||
        sidebar_toggle_bounds_.contains(x, y) || insights_bounds_.contains(x, y))
        return Shape::pointer;
    if (const auto index = area_at(x, y)) {
        const auto& owner = *areas_[*index];
        const auto& tab = *owner.tabs[owner.active];
        if (owner.back.contains(x, y))
            return hand(!tab.history.empty() && tab.here > 0);
        if (owner.forward.contains(x, y))
            return hand(tab.here + 1 < tab.history.size());
        if (owner.tab_bar.claims(x, y))
            return hand(owner.tab_bar.clickable_at(x, y));
        if (owner.breadcrumb.claims(x, y))
            return owner.breadcrumb.editing() ? Shape::text
                                              : hand(owner.breadcrumb.clickable_at(x, y));
    }
    if (sidebar_drawn_ && sidebar_.claims(x, y)) {
        return hand(sidebar_.clickable_at(x, y));
    }
    if (area().tab_bar.claims(x, y)) {
        return hand(area().tab_bar.clickable_at(x, y));
    }
    if (area().breadcrumb.claims(x, y)) {
        return area().breadcrumb.editing() ? Shape::text
                                           : hand(area().breadcrumb.clickable_at(x, y));
    }
    if (status_bar_.claims(x, y)) {
        return hand(status_bar_.clickable_at(x, y));
    }
    if (info_pane_.text_at(x, y))
        return Shape::text;
    if (info_pane_.clickable_at(x, y))
        return Shape::pointer;
    if (const auto index = area_at(x, y)) {
        const auto& owner = *areas_[*index];
        const auto& view = owner.tabs[owner.active]->insights;
        if (view.is_open())
            return view.cursor_at(x, y);
    }
    return Shape::standard;
}

void Browser::take_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.kind == ui::PointerEvent::Kind::press) {
        found_after_.reset();
    }
    if (event.kind == ui::PointerEvent::Kind::release &&
        (errors_.is_open() || password_.is_open() || mount_password_.is_open() ||
         location_.is_open() || batch_rename_.is_open() || conflict_.is_open() ||
         confirm_.is_open() || properties_.is_open() || settings_.is_open() || palette_.is_open() ||
         menu_.is_open() || shortcuts_.is_open() || quick_look_.is_open())) {
        // Taken by what the press opened (a menu from a right click): the button is up all the
        // same, and the next press finds its target again.
        button_held_ = false;
        band_press_.reset();
        end_band();
    }
    if (password_.is_open()) {
        password_.pointer(event);
        return;
    }
    if (errors_.is_open()) {
        errors_.pointer(event);
        return;
    }
    if (conflict_.is_open()) {
        conflict_.pointer(event);  // the question first: nothing under it reacts
        return;
    }
    if (confirm_.is_open()) {
        confirm_.pointer(event);
        return;
    }
    if (mount_password_.is_open()) {
        mount_password_.pointer(event);
        return;
    }
    if (batch_rename_.is_open()) {
        batch_rename_.pointer(event);
        return;
    }
    if (location_.is_open()) {
        location_.pointer(event);
        return;
    }
    if (settings_.is_open()) {
        settings_.pointer(event);
        return;
    }
    if (properties_.is_open()) {
        properties_.pointer(event);
        return;
    }
    if (palette_.is_open()) {
        palette_.pointer(event);  // a click outside closes it and does nothing else
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (menu_.is_open()) {
        menu_.pointer(event);  // a click outside closes it and does nothing else
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (showing_photos() && photo_library_->has_dialog()) {
        photo_library_->pointer_dialog(event);
        return;
    }
    if (searching_results()) {
        if (quick_look_.is_open())
            quick_look_.pointer(event, now);
        else
            search_results_->pointer(event);
        return;
    }
    // While a name is edited, a press in its field is the field's; elsewhere it keeps the name,
    // then does what it does.
    if (sidebar_.rename_pointer(event)) {
        return;  // the same for a bookmark's label
    }
    if (editor_.pointer(event)) {
        return;
    }
    if (shortcuts_.is_open()) {
        shortcuts_.pointer(event);
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (quick_look_.is_open()) {
        // Over the overlay: a click on a text page selects, elsewhere it closes; scrolling
        // moves a text page; the rest waits until it is gone.
        const bool taken = quick_look_.pointer(event, now);
        if (event.kind == ui::PointerEvent::Kind::press && !taken) {
            quick_look_.close();
        }
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::leave) {
        pointer_inside_ = false;
    } else {
        pointer_inside_ = true;
        pointer_x_ = event.x;
        pointer_y_ = event.y;
    }
    if (event.kind == ui::PointerEvent::Kind::press &&
        (event.button == ui::PointerButton::back || event.button == ui::PointerButton::forward)) {
        if (const auto index = area_at(event.x, event.y))
            focus_pane(*index);
        travel(event.button == ui::PointerButton::back ? -1 : 1);
        return;
    }
    dispatch_pointer(event, now);
}

void Browser::dispatch_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    if (resizing_sidebar_preview_) {
        if (event.kind == Kind::motion && sidebar_bounds_.height > 0) {
            sidebar_preview_ratio_ =
                std::clamp((event.y - sidebar_bounds_.y) / sidebar_bounds_.height, 0.15F, 0.85F);
            resize_cursor_ = ui::CursorShape::ns_resize;
            redraw_();
        }
        if (event.kind == Kind::release || event.kind == Kind::leave) {
            resizing_sidebar_preview_ = false;
            resize_cursor_ = ui::CursorShape::standard;
        }
        return;
    }
    if (resizing_split_) {
        if (event.kind == Kind::motion) {
            const float length = (stacked_ ? split_bounds_.height : split_bounds_.width) - 6;
            if (length > 0)
                set_split_ratio(
                    ((stacked_ ? event.y - split_bounds_.y : event.x - split_bounds_.x) - 3) /
                    length);
        }
        if (event.kind == Kind::release || event.kind == Kind::leave) {
            resizing_split_ = false;
            resize_cursor_ = ui::CursorShape::standard;
        }
        return;
    }
    if (resizing_info_) {
        if (event.kind == Kind::motion) {
            info_pane_.set_width(info_resize_width_ + info_resize_x_ - event.x);
            redraw_();
        }
        if (event.kind == Kind::release || event.kind == Kind::leave) {
            resizing_info_ = false;
            resize_cursor_ = ui::CursorShape::standard;
        }
        return;
    }
    if (!button_held_ && event.kind == Kind::press && event.button == ui::PointerButton::left) {
        if (sidebar_preview_divider_.contains(event.x, event.y)) {
            if (event.click_count == 2)
                sidebar_preview_ratio_ = 0.55F;
            else
                resizing_sidebar_preview_ = true;
            redraw_();
            return;
        }
        if (divider_.contains(event.x, event.y)) {
            if (event.click_count == 2)
                set_split_ratio(0.5F);
            else {
                resizing_split_ = true;
                resize_cursor_ = stacked_ ? ui::CursorShape::ns_resize : ui::CursorShape::ew_resize;
            }
            return;
        }
        if (!preview_below_places_ && shared_info_bounds_.width > 0 &&
            std::abs(event.x - shared_info_bounds_.x) <= InfoPane::kEdgeGrab &&
            event.y >= shared_info_bounds_.y && event.y < shared_info_bounds_.bottom()) {
            if (event.click_count == 2) {
                info_pane_.set_width(InfoPane::kWidth);
                redraw_();
            } else {
                resizing_info_ = true;
                info_resize_x_ = event.x;
                info_resize_width_ = info_pane_.width();
                resize_cursor_ = ui::CursorShape::ew_resize;
            }
            return;
        }
    }
    if (!button_held_ && info_pane_.pointer(event, now)) {
        if (event.kind == Kind::press) {
            filter_focused_ = false;
            sidebar_.set_focused(false);
        }
        return;
    }
    if (!button_held_ && shared_info_bounds_.contains(event.x, event.y)) {
        if (event.kind == Kind::scroll)
            info_pane_.scroll(event, now);
        if (event.kind == Kind::press) {
            if (event.button == ui::PointerButton::right)
                open_menu(event.x, event.y, false);
            else if (event.button == ui::PointerButton::left)
                info_pane_.click(event.x, event.y);
        }
        return;
    }
    const auto hovered = area_at(event.x, event.y);
    const std::size_t index =
        button_held_ || event.kind == Kind::leave ? pointer_area_ : hovered.value_or(active_area_);
    if (index != pointer_area_ && areas_[pointer_area_]) {
        AreaContext previous{context_area_, pointer_area_};
        send_pointer(pointer_target_, {.kind = Kind::leave}, now);
        pointer_target_ = Target::view;
    }
    if (event.kind == Kind::press && hovered && !button_held_)
        focus_pane(index);
    pointer_area_ = index;
    AreaContext addressed{context_area_, index};
    if (event.kind == Kind::press && !area().breadcrumb.claims(event.x, event.y))
        area().breadcrumb.cancel_edit();
    dispatch_area_pointer(event, now);
}

void Browser::dispatch_area_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::leave) {
        send_pointer(pointer_target_, event, now);
        pointer_target_ = Target::view;
        button_held_ = false;
        band_press_.reset();
        end_band();
        return;
    }
    // While a button is held (a drag) the pointer stays with whoever took the press.
    if (!button_held_) {
        Target target = Target::view;
        if (sidebar_drawn_ && sidebar_.claims(event.x, event.y)) {
            target = Target::sidebar;
        } else if (area().tab_bar.claims(event.x, event.y)) {
            target = Target::tabs;
        } else if (area().back.contains(event.x, event.y) ||
                   area().forward.contains(event.x, event.y)) {
            target = Target::navigation;
        } else if (area().breadcrumb.claims(event.x, event.y)) {
            target = Target::breadcrumb;
        } else if (toolbar_bounds_.contains(event.x, event.y)) {
            target = Target::toolbar;
        } else if (status_bar_.claims(event.x, event.y)) {
            target = Target::status_bar;
        }
        if (target != pointer_target_) {
            send_pointer(pointer_target_, {.kind = Kind::leave}, now);
            pointer_target_ = target;
        }
    }
    if (event.kind == Kind::press) {
        button_held_ = true;
        if (pointer_target_ != Target::toolbar) {
            filter_focused_ = false;
            redraw_();
        }
        if (pointer_target_ != Target::sidebar && sidebar_.focused()) {
            sidebar_.set_focused(false);  // a click elsewhere takes the keyboard back
        }
    } else if (event.kind == Kind::release) {
        button_held_ = false;
    }
    if (event.kind == Kind::press && event.button == ui::PointerButton::middle &&
        pointer_target_ == Target::view) {
        // A middle click on a folder opens it in a new tab, behind the one shown; the views take
        // no middle clicks.
        if (const std::optional<ViewHit> hit = view_hit(event.x, event.y);
            hit && hit->folder && hit->entry && !hit->folder->is_trash()) {
            const std::vector<fs::Entry>& entries = hit->folder->entries();
            if (*hit->entry < entries.size() && entries[*hit->entry].is_directory()) {
                const fs::Entry& clicked = entries[*hit->entry];
                // On a selected folder: every selected folder, as "Open in New Tabs".
                std::vector<std::filesystem::path> folders;
                if (hit->folder->is_selected(clicked.name)) {
                    for (const fs::Entry& entry : entries) {
                        if (entry.is_directory() && hit->folder->is_selected(entry.key())) {
                            folders.push_back(hit->folder->path() / entry.name);
                        }
                    }
                }
                if (folders.size() > 1) {
                    open_tabs(folders);
                } else {
                    open_tab_behind(hit->folder->path() / clicked.name);
                }
            }
        }
        return;
    }
    if (pointer_target_ == Target::view && !picker_mode_ && dnd_.start) {
        if (event.kind == Kind::press && event.button == ui::PointerButton::right) {
            if (auto hit = view_hit(event.x, event.y); hit && hit->on_item && hit->entry &&
                                                       !hit->folder->is_trash() &&
                                                       !hit->folder->is_archive()) {
                // Keep right click available on release; crossing the drag threshold
                // turns the same press serial into an operation-choice drag.
                press_ = Press{.x = event.x, .y = event.y, .hit = std::move(*hit), .choose = true};
                return;
            }
        } else if (event.kind == Kind::release && event.button == ui::PointerButton::right &&
                   press_ && press_->choose) {
            auto press = event;
            press.kind = Kind::press;
            press.x = press_->x;
            press.y = press_->y;
            press_.reset();
            send_pointer(Target::view, press, now);
            send_pointer(Target::view, event, now);
            return;
        }
    }
    if (event.kind == Kind::motion && press_ && dnd_.start && button_held_ &&
        pointer_target_ == Target::view &&
        std::hypot(event.x - press_->x, event.y - press_->y) >= kDragThreshold) {
        Press press = std::move(*press_);
        press_.reset();
        start_drag(std::move(press));
        return;
    }
    if (event.kind == Kind::motion && band_) {
        band_->x = event.x;
        band_->y = event.y;
        update_band();
        return;  // the views see nothing meanwhile (no hover)
    }
    if (event.kind == Kind::motion && band_press_ && button_held_ &&
        pointer_target_ == Target::view &&
        std::hypot(event.x - band_press_->x, event.y - band_press_->y) >= kDragThreshold) {
        start_band(event.x, event.y);
        return;
    }
    send_pointer(pointer_target_, event, now);
    if (event.kind == Kind::scroll && band_) {
        update_band();  // the list moved under it
    }
    if (event.kind == Kind::press) {
        press_.reset();
        band_press_.reset();
        end_band();
        if (pointer_target_ == Target::view && event.button == ui::PointerButton::left &&
            event.click_count == 1) {
            // On an entry's icon or name, selected by the press just handled, a drag may start;
            // on empty space, or beside the name, a band.
            if (std::optional<ViewHit> hit = view_hit(event.x, event.y); hit && hit->on_item) {
                press_ = Press{.x = event.x, .y = event.y, .hit = std::move(*hit)};
            } else if (hit) {
                band_press_ = BandPress{.x = event.x,
                                        .y = event.y,
                                        .folder = std::move(hit->folder),
                                        .add = event.modifiers.ctrl};
            }
        }
    } else if (event.kind == Kind::release && event.button == ui::PointerButton::left) {
        if (press_) {
            press_->hit.folder->release_press();
            press_.reset();
        }
        if (band_press_) {
            band_press_->folder->release_press();
            band_press_.reset();
        }
        end_band();
    }
}

std::optional<BandLane> Browser::band_lane(const Folder& folder) const {
    switch (pane().mode) {
        case Mode::grid:
            return pane().grid.band_lane(folder);
        case Mode::details:
            return pane().details.band_lane(folder);
        case Mode::columns:
            break;
    }
    return pane().columns.band_lane(folder);
}

void Browser::start_band(float x, float y) {
    if (!band_press_) {
        return;
    }
    BandPress press = std::move(*band_press_);
    band_press_.reset();
    const std::optional<BandLane> lane = band_lane(*press.folder);
    if (!lane) {
        return;
    }
    std::vector<std::string> base;
    if (press.add) {
        base = press.folder->selected_names();
    }
    band_ = Band{.folder = std::move(press.folder),
                 .anchor_x = press.x - lane->area.x,
                 .anchor_y = press.y - lane->area.y + lane->offset,
                 .base = std::move(base),
                 .x = x,
                 .y = y};
    update_band();
}

void Browser::update_band() {
    if (!band_) {
        return;
    }
    Band& band = *band_;
    const std::optional<BandLane> lane = band_lane(*band.folder);
    if (!lane || lane->area.width <= 0.0F || lane->area.height <= 0.0F) {
        end_band();  // its folder is not shown any more
        return;
    }
    const ui::Rect& area = lane->area;
    const float x = std::clamp(band.x, area.x, area.right()) - area.x;
    const float y = std::clamp(band.y, area.y, area.bottom()) - area.y + lane->offset;
    const ui::Rect content{std::min(x, band.anchor_x), std::min(y, band.anchor_y),
                           std::abs(x - band.anchor_x), std::abs(y - band.anchor_y)};
    std::vector<std::size_t> touched;
    switch (pane().mode) {
        case Mode::grid:
            touched = pane().grid.band_entries(*band.folder, content);
            break;
        case Mode::details:
            touched = pane().details.band_entries(*band.folder, content);
            break;
        case Mode::columns:
            touched = pane().columns.band_entries(*band.folder, content);
            break;
    }
    band.folder->select_band(band.base, touched);
    redraw_();
    // Beyond the top or the bottom of the list, it scrolls on its own.
    const bool beyond = band.y < area.y || band.y >= area.bottom();
    if (beyond && band_timer_ == 0) {
        band_timer_ = loop_.add_timer(std::chrono::milliseconds{16}, std::chrono::milliseconds{16},
                                      [this] { scroll_band(); });
    } else if (!beyond && band_timer_ != 0) {
        loop_.remove(band_timer_);
        band_timer_ = 0;
    }
}

void Browser::scroll_band() {
    const std::optional<BandLane> lane = band_ ? band_lane(*band_->folder) : std::nullopt;
    if (!lane) {
        end_band();
        return;
    }
    // Faster the farther the pointer is: a pixel a tick per 4 pixels beyond, up to 40.
    const float beyond =
        band_->y < lane->area.y ? band_->y - lane->area.y : band_->y - lane->area.bottom();
    const float step = std::clamp(beyond / 4.0F, -40.0F, 40.0F);
    const float pixels = step < 0.0F ? std::min(step, -2.0F) : std::max(step, 2.0F);
    Folder& folder = *band_->folder;
    switch (pane().mode) {
        case Mode::grid:
            pane().grid.band_scroll(folder, pixels);
            break;
        case Mode::details:
            pane().details.band_scroll(folder, pixels);
            break;
        case Mode::columns:
            pane().columns.band_scroll(folder, pixels);
            break;
    }
    update_band();
}

void Browser::end_band() {
    loop_.remove(band_timer_);
    band_timer_ = 0;
    if (band_) {
        band_.reset();
        redraw_();
    }
}

void Browser::draw_band(ui::Renderer& renderer, const theme::Palette& palette) const {
    if (!band_) {
        return;
    }
    const std::optional<BandLane> lane = band_lane(*band_->folder);
    if (!lane) {
        return;
    }
    const ui::Rect& area = lane->area;
    const float left = area.x + band_->anchor_x;
    const float top = area.y + band_->anchor_y - lane->offset;
    const float x = std::clamp(band_->x, area.x, area.right());
    const float y = std::clamp(band_->y, area.y, area.bottom());
    renderer.push_clip(area);
    renderer.draw_box({std::min(left, x), std::min(top, y), std::abs(x - left), std::abs(y - top)},
                      // The text's colour: it shows over the selection it makes, which has
                      // the accent's.
                      {.fill = palette.on_surface.with_alpha(0.12F),
                       .radius = {},
                       .border_width = 1.5F,
                       .border = palette.on_surface.with_alpha(0.85F)});
    renderer.pop_clip();
}

void Browser::send_pointer(Target target, const ui::PointerEvent& event, Clock::time_point now) {
    switch (target) {
        case Target::view:
            view_pointer(event, now);
            break;
        case Target::sidebar:
            sidebar_.pointer(event, now);
            break;
        case Target::breadcrumb:
            area().breadcrumb.pointer(event);
            break;
        case Target::status_bar:
            status_bar_.pointer(event);
            break;
        case Target::tabs:
            area().tab_bar.pointer(event);
            break;
        case Target::toolbar:
            toolbar_pointer(event);
            break;
        case Target::navigation:
            navigation_pointer(event);
            break;
    }
}

void Browser::go_to(const std::filesystem::path& folder, const std::string& child) {
    if (pane().insights.is_open()) {
        pane().insights.navigate(folder);
        return;
    }
    ++path_navigation_generation_;
    if (pane().mode == Mode::columns) {
        pane().restore_mode_on_arrival = true;
        if (!pane().columns.focus_folder(folder)) {
            pane().columns.reveal(folder, child.empty() ? folder : folder / child);
        }
        redraw_();
    } else {
        open(folder);
        if (const std::shared_ptr<Folder> shown = current_folder(); shown && !child.empty()) {
            shown->select_name(child);
        }
    }
    note_location();
}

std::optional<ViewHit> Browser::view_hit(float x, float y) const {
    const auto index = area_at(x, y);
    const auto& owner = index ? *areas_[*index] : area();
    const auto& tab = *owner.tabs[owner.active];
    if (tab.insights.is_open())
        return tab.insights.hit(x, y);
    switch (tab.mode) {
        case Mode::grid:
            return tab.grid.hit(x, y);
        case Mode::details:
            return tab.details.hit(x, y);
        case Mode::columns:
            return tab.columns.hit(x, y);
    }
    return {};
}

void Browser::start_drag(Press press) {
    Folder& folder = *press.hit.folder;
    folder.forget_press();
    if (folder.is_collection()) {
        show_message(
            std::string{i18n::tr(
                "Photo Library drag is unavailable. Use Copy or Cut, then Paste in a folder.")},
            false);
        return;
    }
    if (folder.is_trash() || folder.is_archive() || !press.hit.entry ||
        *press.hit.entry >= folder.entries().size()) {
        return;  // the trash only gives back (Restore); an archive's entries are extracted
    }
    const fs::Entry& entry = folder.entries()[*press.hit.entry];
    std::vector<std::string> names = folder.is_selected(entry.key())
                                         ? folder.selected_names()
                                         : std::vector<std::string>{entry.name};
    fs::Entry typed = entry;
    typed.mime = folder.mime_of(entry);
    std::vector<std::filesystem::path> paths;
    paths.reserve(names.size());
    for (const std::string& name : names) {
        paths.push_back(folder.path() / name);
    }
    const bool selected = folder.is_selected(entry.key());
    const bool folders_only = std::ranges::none_of(folder.entries(), [&](const fs::Entry& listed) {
        return !listed.is_directory() &&
               (selected ? folder.is_selected(listed.key()) : listed.key() == entry.key());
    });
    drag_ = Drag{.directory = folder.path(),
                 .names = std::move(names),
                 .icon = std::string{fs::icon_name(folder.mime(), typed)},
                 .folders_only = folders_only,
                 .choose = press.choose};
    // The view lets go of the press: the drag has the pointer now.
    send_pointer(Target::view, {.kind = ui::PointerEvent::Kind::leave}, Clock::now());
    button_held_ = false;
    const std::string lines = path_lines(paths);
    dnd_.start({{std::string{kUriList}, uri_list(paths)},
                {std::string{kText}, lines},
                {"text/plain", lines}});
    redraw_();
}

void Browser::drag_ended() {
    drag_.reset();
    show_drop_target(std::nullopt);
    stop_auto_scroll();
}

void Browser::draw_drag_icon(ui::Renderer& renderer, const ui::Rect& bounds,
                             const theme::Palette& palette, ui::IconSet& icons) const {
    renderer.draw_box(
        bounds,
        {.fill = palette.surface_container_high, .border_width = 1.0F, .border = palette.primary});
    if (!drag_) {
        return;
    }
    constexpr float icon_size = 20.0F;
    constexpr float text_size = 13.0F;
    const float left = bounds.x + 10.0F;
    icons.draw(renderer, drag_->icon, left, bounds.y + ((bounds.height - icon_size) / 2.0F),
               icon_size,
               theme::readable_foreground(palette.primary, palette.surface_container_high));
    const std::string label = drag_->names.size() == 1
                                  ? drag_->names.front()
                                  : i18n::trn("{} item", "{} items", drag_->names.size());
    const float text_x = left + icon_size + 8.0F;
    renderer.draw_text(label, text_x, bounds.y + ((bounds.height - text_size) / 2.0F) - 2.0F,
                       text_size, palette.on_surface,
                       std::max(1.0F, bounds.right() - 10.0F - text_x), ui::Ellipsis::middle);
}

std::optional<Browser::DropTarget> Browser::drop_target_at(float x, float y) const {
    if (sidebar_drawn_) {
        if (sidebar_.in_bookmark_zone(x, y)) {
            return DropTarget{.folder = {}, .mark = {}, .place = {}, .crumb = {}, .bookmark = true};
        }
        if (const fs::Place* const place = sidebar_.place_under(x, y)) {
            if (place->path.empty()) {
                return std::nullopt;  // a disk not mounted: nowhere to put anything yet
            }
            return DropTarget{.folder = place->path, .mark = {}, .place = place->path, .crumb = {}};
        }
        if (sidebar_.claims(x, y)) {
            return std::nullopt;
        }
    }
    for (std::size_t i = 0; i < areas_.size(); ++i) {
        if (!areas_[i] || (!split_visible_ && i != active_area_))
            continue;
        const auto& tab = *areas_[i]->tabs[areas_[i]->active];
        if (tab.photos)
            continue;
        if (tab.mode == Mode::details && tab.details.horizontal_scrollbar_bounds().contains(x, y)) {
            const auto folder = tab.details.root_folder();
            if (folder && !folder->is_trash() && !folder->is_archive())
                return DropTarget{.folder = folder->path(),
                                  .mark = {.directory = folder->path(), .name = {}},
                                  .place = {},
                                  .crumb = {}};
        }
        if (const auto crumb = areas_[i]->breadcrumb.path_at(x, y)) {
            const auto& shown_pane = *areas_[i]->tabs[areas_[i]->active];
            const auto shown_folder =
                shown_pane.mode == Mode::columns
                    ? shown_pane.columns.focused_folder()
                    : (shown_pane.mode == Mode::details ? shown_pane.details.folder()
                                                        : shown_pane.grid.folder());
            if (shown_folder && shown_folder->is_archive()) {
                const auto& archive = shown_folder->model().archive_location().archive;
                const auto relative = crumb->lexically_relative(archive);
                if (!relative.empty() && *relative.begin() != "..")
                    return std::nullopt;
            }
            return DropTarget{.folder = *crumb, .mark = {}, .place = {}, .crumb = crumb};
        }
    }
    const std::optional<ViewHit> hit = view_hit(x, y);
    if (!hit || hit->folder->is_collection() || hit->folder->is_trash() ||
        hit->folder->is_archive()) {
        return std::nullopt;
    }
    if (hit->entry && *hit->entry < hit->folder->entries().size()) {
        const fs::Entry& entry = hit->folder->entries()[*hit->entry];
        if (entry.is_directory()) {
            return DropTarget{.folder = hit->folder->path() / entry.name,
                              .mark = {.directory = hit->folder->path(), .name = entry.name},
                              .place = {},
                              .crumb = {}};
        }
    }
    return DropTarget{.folder = hit->folder->path(),
                      .mark = {.directory = hit->folder->path(), .name = {}},
                      .place = {},
                      .crumb = {}};
}

std::string Browser::refusal(const std::filesystem::path& folder,
                             const std::vector<std::filesystem::path>& paths) {
    if (fs::is_trash(folder)) {
        return {};
    }
    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path inside = folder.lexically_relative(path);
        if (!inside.empty() && *inside.begin() != "..") {
            return i18n::trf("“{}” cannot go into itself", path.filename().string());
        }
    }
    if (std::ranges::all_of(paths, [&folder](const std::filesystem::path& path) {
            return path.parent_path() == folder;
        })) {
        return std::string{i18n::tr("Already here")};
    }
    return {};
}

void Browser::show_drop_target(const std::optional<DropTarget>& target) {
    DropMark mark = target ? target->mark : DropMark{};
    if (mark.directory != drop_mark_.directory || mark.name != drop_mark_.name) {
        drop_mark_ = std::move(mark);
        redraw_();
    }
    sidebar_.set_drop_target(target ? target->place : std::nullopt);
    sidebar_.set_bookmark_drop(target && target->bookmark);
    for (auto& owner : areas_)
        if (owner)
            owner->breadcrumb.set_drop_target(target ? target->crumb : std::nullopt);
}

void Browser::drop_event(const ui::DropEvent& event) {
    if (searching_results()) {
        show_drop_target(std::nullopt);
        stop_auto_scroll();
        if (dnd_.accept)
            dnd_.accept(std::nullopt, 0, 0);
        return;
    }
    AreaContext addressed{context_area_, area_at(event.x, event.y).value_or(active_area_)};
    using Kind = ui::DropEvent::Kind;
    const bool choose =
        !picker_mode_ && ((drag_ && drag_->choose) || (dnd_.modifiers && dnd_.modifiers().alt));
    drop_x_ = event.x;
    drop_y_ = event.y;
    if (event.kind == Kind::leave) {
        show_drop_target(std::nullopt);
        stop_auto_scroll();
        return;
    }
    std::optional<DropTarget> target = drop_target_at(event.x, event.y);
    const std::vector<std::string> types = dnd_.types ? dnd_.types() : std::vector<std::string>{};
    const bool files = std::ranges::find(types, kUriList) != types.end();
    const auto content = content_offer(types);
    const auto offered = files || drag_ ? std::optional{std::string{kUriList}} : content;
    std::vector<std::filesystem::path> own;
    if (drag_) {
        for (const std::string& name : drag_->names) {
            own.push_back(drag_->directory / name);
        }
        if (target &&
            (target->bookmark ? !drag_->folders_only
                              : (!refusal(target->folder, own).empty() &&
                                 !(choose && std::ranges::all_of(own, [&](const auto& path) {
                                       return path.parent_path() == target->folder;
                                   }))))) {
            target.reset();  // not there: nothing would happen, or it cannot be
        }
    } else if (!offered ||
               (target && !files && (target->bookmark || fs::is_trash(target->folder)))) {
        target.reset();
    }
    if (event.kind == Kind::drop) {
        show_drop_target(std::nullopt);
        stop_auto_scroll();
        if (drag_) {
            if (dnd_.finish) {
                dnd_.finish();
            }
            if (target && target->bookmark) {
                add_to_sidebar(std::move(own));
            } else if (target) {
                drop_paths(target->folder, std::move(own), true, false, choose);
            }
            return;
        }
        if (!target || !dnd_.read) {
            return;
        }
        const bool move = dnd_.action && dnd_.action() == ui::kDndMove;
        const bool move_allowed =
            dnd_.source_actions && (dnd_.source_actions() & ui::kDndMove) != 0;
        if (choose && move) {
            // Reading finishes the Wayland offer. A negotiated move could then delete the
            // source before the user chooses extraction or cancels the menu.
            show_message(
                std::string{i18n::tr("Hold Alt before entering the destination, then drop again")},
                true);
            return;
        }
        const auto shown = current_folder();
        const auto reveal = shown && shown->path() == target->folder ? shown : nullptr;
        ++operations_;
        dnd_.read(*offered, [this, alive = alive_, folder = target->folder,
                             bookmark = target->bookmark, move, choose, move_allowed, files,
                             content, type = *offered, reveal](std::optional<std::string> data) {
            if (!*alive)
                return;
            --operations_;
            if (!data || data->size() > kMaxContentBytes) {
                show_message(std::string{i18n::tr("Nothing could be read from the drop")}, true);
                return;
            }
            if (!files) {
                create_contents(folder, type, std::move(*data), true, reveal);
                return;
            }
            ClipboardFiles dropped = parse_uri_list(*data);
            if (!dropped.paths.empty()) {
                if (bookmark)
                    add_to_sidebar(std::move(dropped.paths));
                else if (choose && !fs::is_trash(folder))
                    drop_actions(folder, std::move(dropped.paths), false, move_allowed);
                else
                    drop_paths(folder, std::move(dropped.paths), false, move, choose);
                return;
            }
            if (!bookmark && !fs::is_trash(folder) && !move) {
                // URI lists may be web links. Local files win, then image bytes, then a URL.
                if (content && content->starts_with("image/")) {
                    ++operations_;
                    dnd_.read(*content, [this, alive, folder, reveal,
                                         type = *content](std::optional<std::string> image) {
                        if (!*alive)
                            return;
                        --operations_;
                        if (image)
                            create_contents(folder, type, std::move(*image), true, reveal);
                        else
                            show_message(
                                std::string{i18n::tr("Nothing could be read from the drop")}, true);
                    });
                    return;
                }
                if (dropped_web_url(*data)) {
                    create_contents(folder, std::string{kUriList}, std::move(*data), true, reveal);
                    return;
                }
            }
            show_message(
                std::string{dropped.refused > 0 ? i18n::tr("Only files can be dropped")
                                                : i18n::tr("Nothing could be read from the drop")},
                false);
        });
        return;
    }
    show_drop_target(target);
    if (dnd_.accept) {
        if (!target) {
            dnd_.accept(std::nullopt, 0, 0);
        } else if (drag_ || choose || !files) {
            dnd_.accept(offered, ui::kDndCopy, ui::kDndCopy);  // decided at the drop
        } else {
            const std::uint32_t allowed = dnd_.source_actions ? dnd_.source_actions() : 0;
            dnd_.accept(std::string{kUriList}, ui::kDndCopy | (allowed & ui::kDndMove),
                        ui::kDndCopy);
        }
    }
    // Near the top or the bottom of the view, it scrolls.
    float notches = 0.0F;
    if (event.x >= area().view_area.x && event.x < area().view_area.right()) {
        if (event.y >= area().view_area.y && event.y < area().view_area.y + kAutoScrollEdge) {
            notches = -0.3F;
        } else if (event.y < area().view_area.bottom() &&
                   event.y >= area().view_area.bottom() - kAutoScrollEdge) {
            notches = 0.3F;
        }
    }
    auto_scroll_ = notches;
    if (notches == 0.0F) {
        stop_auto_scroll();
    } else if (auto_scroll_timer_ == 0) {
        auto_scroll_timer_ =
            loop_.add_timer(std::chrono::milliseconds{0}, std::chrono::milliseconds{40},
                            [this] { scroll_near_edge(); });
    }
}

void Browser::scroll_near_edge() {
    AreaContext addressed{context_area_, area_at(drop_x_, drop_y_).value_or(active_area_)};
    if (auto_scroll_ == 0.0F) {
        stop_auto_scroll();
        return;
    }
    view_pointer({.kind = ui::PointerEvent::Kind::scroll,
                  .x = drop_x_,
                  .y = drop_y_,
                  .wheel_y = auto_scroll_},
                 Clock::now());
    redraw_();
}

void Browser::stop_auto_scroll() {
    auto_scroll_ = 0.0F;
    loop_.remove(auto_scroll_timer_);
    auto_scroll_timer_ = 0;
}

void Browser::drop_paths(const std::filesystem::path& folder,
                         std::vector<std::filesystem::path> paths, bool own, bool move,
                         bool choose) {
    if (choose && !picker_mode_ && !fs::is_trash(folder)) {
        drop_actions(folder, std::move(paths), own);
        return;
    }
    if (fs::is_trash(folder)) {
        trash_paths(paths, Purpose::user);
        return;
    }
    if (const std::string why = refusal(folder, paths); !why.empty()) {
        show_message(why, false);
        return;
    }
    if (!own) {
        transfer_paths(folder, paths, move, std::nullopt);
        return;
    }
    const ui::Modifiers modifiers = dnd_.modifiers ? dnd_.modifiers() : ui::Modifiers{};
    if (modifiers.ctrl || modifiers.shift) {
        transfer_paths(folder, paths, modifiers.shift && !modifiers.ctrl, std::nullopt);
        return;
    }
    // On the same disk it moves, onto another it copies: which disk is a stat, on a worker.
    ++operations_;
    std::filesystem::path from = paths.front().parent_path();  // before paths moves away
    pool_.submit(
        core::Priority::visible,
        [folder, from = std::move(from)](const core::CancelToken& /*token*/) {
            struct stat to_stat{};
            struct stat from_stat{};
            return ::stat(folder.c_str(), &to_stat) == 0 && ::stat(from.c_str(), &from_stat) == 0 &&
                   to_stat.st_dev == from_stat.st_dev;
        },
        [this, alive = alive_, folder, paths = std::move(paths)](bool same_disk) {
            if (!*alive) {
                return;
            }
            --operations_;
            transfer_paths(folder, paths, same_disk, std::nullopt);
        });
}

void Browser::add_to_sidebar(std::vector<std::filesystem::path> folders) {
    std::erase_if(folders, [](const std::filesystem::path& path) { return fs::is_trash(path); });
    if (folders.empty()) {
        show_message(std::string{i18n::tr("Already in the sidebar")}, false);
        return;
    }
    BookmarkChange change;
    change.add = std::move(folders);
    bookmark_changes_.push_back(std::move(change));
    next_bookmark_change();
}

void Browser::remove_from_sidebar(const std::filesystem::path& folder) {
    BookmarkChange change;
    change.remove = folder;
    bookmark_changes_.push_back(std::move(change));
    next_bookmark_change();
}

void Browser::rename_bookmark(const std::filesystem::path& folder, std::string label) {
    BookmarkChange change;
    change.rename = std::pair{folder, std::move(label)};
    bookmark_changes_.push_back(std::move(change));
    next_bookmark_change();
}

void Browser::repoint_bookmark(const std::filesystem::path& from, std::filesystem::path to) {
    BookmarkChange change;
    change.repoint = std::pair{from, std::move(to)};
    bookmark_changes_.push_back(std::move(change));
    next_bookmark_change();
}

void Browser::move_bookmark(const std::filesystem::path& folder, int step) {
    BookmarkChange change;
    change.move = std::pair{folder, step};
    bookmark_changes_.push_back(std::move(change));
    next_bookmark_change();
}

void Browser::remove_remote_from_sidebar(const std::string& uri, const std::string& label) {
    bookmark_changes_.push_back(
        {.add = {}, .remove = std::nullopt, .remove_uri = uri, .remove_label = label});
    next_bookmark_change();
}

void Browser::next_bookmark_change() {
    if (bookmark_busy_ || bookmark_changes_.empty()) {
        return;
    }
    BookmarkChange change = std::move(bookmark_changes_.front());
    bookmark_changes_.pop_front();
    bookmark_busy_ = true;
    ++operations_;

    // What the change did, for the message.
    struct Outcome {
        std::vector<std::string> added;
        std::size_t present{0};
        std::size_t not_folders{0};
        std::string removed;
        std::string error;
        std::string edited;  // what a rename or a new folder did, for the message
        bool changed{false};
        std::optional<std::filesystem::path> follow;  // where the sidebar's cursor goes now
        std::optional<std::filesystem::path> repoint_from;
    };

    pool_.submit(
        core::Priority::visible,
        [file = bookmarks_file_.value_or(fs::bookmarks_file(place_sources_)),
         change = std::move(change)](const core::CancelToken& /*token*/) {
            Outcome outcome;
            core::TextFile read = core::read_text_file(file);
            if (!read.error.empty()) {
                outcome.error = read.error;
                return outcome;
            }
            std::string text = std::move(read.text);
            bool changed = false;
            for (const std::filesystem::path& folder : change.add) {
                std::error_code ignored;
                if (!std::filesystem::is_directory(folder, ignored)) {
                    ++outcome.not_folders;
                } else if (std::optional<std::string> more = fs::add_bookmark(text, folder)) {
                    text = std::move(*more);
                    changed = true;
                    std::filesystem::path normal = folder.lexically_normal();
                    if (!normal.has_filename()) {
                        normal = normal.parent_path();  // a trailing slash
                    }
                    outcome.added.push_back(normal.filename().string());
                } else {
                    ++outcome.present;
                }
            }
            if (change.remove) {
                if (std::optional<std::string> less = fs::remove_bookmark(text, *change.remove)) {
                    text = std::move(*less);
                    changed = true;
                    outcome.removed = change.remove->filename().string();
                }
            }
            if (change.add_uri) {
                if (std::optional<std::string> more =
                        fs::add_remote_bookmark(text, *change.add_uri)) {
                    text = std::move(*more);
                    changed = true;
                    outcome.added.push_back(change.add_label);
                }  // there already: nothing to say, the connecting says enough
            }
            if (change.remove_uri) {
                if (std::optional<std::string> less =
                        fs::remove_remote_bookmark(text, *change.remove_uri)) {
                    text = std::move(*less);
                    changed = true;
                    outcome.removed = change.remove_label;
                }
            }
            if (change.rename) {
                const auto& [folder, label] = *change.rename;
                if (std::optional<std::string> renamed = fs::rename_bookmark(text, folder, label);
                    renamed && *renamed != text) {
                    text = std::move(*renamed);
                    changed = true;
                    const std::string shown = label.empty() ? folder.filename().string() : label;
                    outcome.edited = i18n::trf("Renamed “{}” in the sidebar", shown);
                }
            }
            if (change.repoint) {
                const auto& [from, to] = *change.repoint;
                std::error_code ignored;
                const std::vector<fs::Place> bookmarks = fs::parse_bookmarks(text);
                const bool there = std::ranges::any_of(bookmarks, [&](const fs::Place& place) {
                    return place.path.lexically_normal() == to.lexically_normal();
                });
                if (!std::filesystem::is_directory(to, ignored)) {
                    ++outcome.not_folders;
                } else if (there) {
                    ++outcome.present;
                } else if (std::optional<std::string> moved =
                               fs::repoint_bookmark(text, from, to)) {
                    text = std::move(*moved);
                    changed = true;
                    outcome.edited = i18n::trf("The bookmark now opens “{}”",
                                               to.lexically_normal().filename().string());
                    outcome.follow = to.lexically_normal();
                    outcome.repoint_from = from;
                }
            }
            if (change.move) {
                const auto& [folder, step] = *change.move;
                if (std::optional<std::string> moved = fs::move_bookmark(text, folder, step)) {
                    text = std::move(*moved);
                    changed = true;  // the sidebar shows it: no message
                }
            }
            outcome.changed = changed;
            if (changed) {
                if (const std::error_code error = core::write_file_atomically(file, text)) {
                    outcome.error = error.message();
                    outcome.added.clear();
                    outcome.removed.clear();
                }
            }
            return outcome;
        },
        [this, alive = alive_](const Outcome& outcome) {
            if (!*alive) {
                return;
            }
            --operations_;
            bookmark_busy_ = false;
            if (!outcome.error.empty()) {
                show_message(i18n::trf("The sidebar could not be changed: {}", outcome.error),
                             true);
            } else if (outcome.added.size() == 1) {
                show_message(i18n::trf("“{}” added to the sidebar", outcome.added.front()), false);
            } else if (!outcome.added.empty()) {
                show_message(i18n::trn("{} folder added to the sidebar",
                                       "{} folders added to the sidebar", outcome.added.size()),
                             false);
            } else if (!outcome.removed.empty()) {
                show_message(i18n::trf("“{}” removed from the sidebar", outcome.removed), false);
            } else if (!outcome.edited.empty()) {
                show_message(outcome.edited, false);
            } else if (outcome.present > 0) {
                show_message(std::string{i18n::tr("Already in the sidebar")}, false);
            } else if (outcome.not_folders > 0) {
                show_message(std::string{i18n::tr("Only folders go in the sidebar")}, false);
            }
            if (outcome.changed && outcome.error.empty() && outcome.repoint_from &&
                outcome.follow) {
                const auto old_key = "path:" + outcome.repoint_from->string();
                const auto new_key = "path:" + outcome.follow->string();
                const auto replace = [&](std::vector<std::string> values) {
                    std::replace(values.begin(), values.end(), old_key, new_key);
                    return values;
                };
                sidebar_.set_organization(replace(sidebar_.order()), replace(sidebar_.hidden()),
                                          sidebar_.collapsed());
            }
            if (outcome.changed && outcome.error.empty() && places_reload_) {
                places_reload_();
                if (outcome.follow) {
                    sidebar_.set_cursor_to(*outcome.follow);
                }
            }
            next_bookmark_change();
        });
}

void Browser::open_place(const fs::Place& place, PlaceDestination destination) {
    if (Sidebar::separator(place))
        return;
    if (place.uri == "rummage:photos" || place.uri.starts_with("rummage:photos/")) {
        if (destination != PlaceDestination::current || !photos_available())
            return;
        sidebar_.set_focused(false);
        open_photo_library();
        if (place.uri == "rummage:photos")
            return;
        for (const auto& [uri, target] :
             {std::pair{"rummage:photos/albums", PhotoLibrary::Destination::albums},
              std::pair{"rummage:photos/sources", PhotoLibrary::Destination::sources},
              std::pair{"rummage:photos/searches", PhotoLibrary::Destination::filters},
              std::pair{"rummage:photos/review", PhotoLibrary::Destination::review},
              std::pair{"rummage:photos/activity", PhotoLibrary::Destination::activity}}) {
            if (place.uri == uri) {
                photo_library_->show(target);
                return;
            }
        }
        for (const auto& [key, prefix] : {std::pair{"albums", "rummage:photos/albums/"},
                                          std::pair{"sources", "rummage:photos/sources/"},
                                          std::pair{"filters", "rummage:photos/searches/"}}) {
            if (!place.uri.starts_with(prefix))
                continue;
            for (const auto& record : photo_navigation_.value(key, nlohmann::json::array())) {
                const auto id = record.value("id", std::int64_t{});
                if (place.uri != std::string{prefix} + std::to_string(id))
                    continue;
                if (std::string_view{key} == "albums")
                    photo_library_->open_album(id, record.value("name", ""));
                else if (std::string_view{key} == "sources")
                    photo_library_->open_source(id, record.value("path", ""));
                else
                    photo_library_->open_saved_search(record);
                return;
            }
            show_message(std::string{i18n::tr("This Photo Library entry is no longer available")},
                         false);
            photo_library_->refresh_navigation();
            return;
        }
        return;
    }
    if (place.uri == "rummage:saved-searches") {
        if (destination == PlaceDestination::current)
            open_saved_searches();
        return;
    }
    if (destination != PlaceDestination::current && picker_mode_)
        return;
    if (destination == PlaceDestination::tab && !panes_enabled_)
        return;
    if (destination == PlaceDestination::window && !new_window_)
        return;
    auto* origin = &area();
    const auto ready = [this, destination, origin](const std::filesystem::path& path) {
        if (path.empty()) {
            show_message(std::string{i18n::tr("Location is unavailable")}, true);
            return;
        }
        if (destination == PlaceDestination::window) {
            if (new_window_)
                new_window_(path);
            return;
        }
        if (destination == PlaceDestination::tab) {
            for (std::size_t side = 0; side < areas_.size(); ++side) {
                if (areas_[side].get() != origin)
                    continue;
                AreaContext addressed{context_area_, side};
                open_tab(path, side == active_area_);
                return;
            }
            return;
        }
        open(path);
    };
    if (!place.disk.mounted) {
        if (!place.uri.empty())
            mount_remote(place, destination == PlaceDestination::current,
                         destination == PlaceDestination::current
                             ? std::function<void(const std::filesystem::path&)>{}
                             : ready);
        else
            mount_place(place, destination == PlaceDestination::current,
                        destination == PlaceDestination::current
                            ? std::function<void(const std::filesystem::path&)>{}
                            : ready);
    } else
        ready(place.path);
}

void Browser::mount_place(const fs::Place& place, bool open,
                          std::function<void(const std::filesystem::path&)> ready) {
    if (!devices_.mount || place.disk.volume.empty()) {
        return;
    }
    show_message(i18n::trf("Mounting “{}”…", place.label), false);
    ++operations_;
    devices_.mount(place.disk.volume, [this, alive = alive_, name = place.label, open,
                                       ready = std::move(ready), origin = current_folder()](
                                          const std::filesystem::path& where,
                                          const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (!error.empty()) {
            show_message(i18n::trf("“{}” could not be mounted: {}", name, i18n::tr(error)), true);
            return;
        }
        if (ready)
            ready(where);
        if (open) {
            // A slow mount belongs to the tab that requested it, even after focus changes.
            open_mounted_folder(origin, where);
        }
        // A disk mounted read-only says why: NTFS left unclean by Windows or by a pull.
        ++operations_;
        pool_.submit(
            core::Priority::visible,
            [where](const core::CancelToken& /*token*/) {
                struct statvfs info{};
                return ::statvfs(where.c_str(), &info) == 0 && (info.f_flag & ST_RDONLY) != 0;
            },
            [this, alive, name](bool read_only) {
                if (!*alive) {
                    return;
                }
                --operations_;
                show_message(read_only ? i18n::trf("“{}” is read-only: it was not closed "
                                                   "properly (Windows' chkdsk or ntfsfix -d "
                                                   "fixes it)",
                                                   name)
                                       : i18n::trf("“{}” mounted", name),
                             read_only);
            });
    });
}

void Browser::mount_remote(const fs::Place& place, bool open,
                           std::function<void(const std::filesystem::path&)> ready) {
    if (place.uri.empty()) {
        return;
    }
    if (!remotes_.mount) {
        show_message(i18n::trf("“{}” could not be mounted: gvfs is not available", place.label),
                     true);
        return;
    }
    show_message(i18n::trf("Connecting to “{}”…", place.label), false);
    ++operations_;
    Remotes::Prompts prompts{
        .login =
            [this, alive = alive_, name = place.label](
                const LoginRequest& request,
                const std::function<void(std::optional<Login>)>& answer) {
                if (!*alive) {
                    answer(std::nullopt);
                    return;
                }
                mount_password_.open(i18n::trf("Connect to “{}”", name), request, answer);
            },
        .question =
            [this, alive = alive_](const std::string& message,
                                   const std::vector<std::string>& choices,
                                   const std::function<void(std::optional<std::size_t>)>& answer) {
                if (!*alive || choices.empty()) {
                    answer(std::nullopt);
                    return;
                }
                // The first line is the question, the rest says why.
                const std::size_t line = message.find('\n');
                confirm_.open(message.substr(0, line),
                              line == std::string::npos ? std::string{} : message.substr(line + 1),
                              choices.front(), [answer](bool confirmed) {
                                  answer(confirmed ? std::optional<std::size_t>{0} : std::nullopt);
                              });
            },
        .cancel =
            [this, alive = alive_] {
                if (*alive) {
                    mount_password_.close();
                    confirm_.close();
                }
            },
    };
    remotes_.mount(
        place.uri, std::move(prompts),
        [this, alive = alive_, name = place.label, open, ready = std::move(ready),
         origin = current_folder()](const std::filesystem::path& where, const std::string& error) {
            if (!*alive) {
                return;
            }
            --operations_;
            mount_password_.close();
            if (error == "cancelled") {
                show_message(i18n::trf("Not connected to “{}”", name), false);
                return;
            }
            if (!error.empty()) {
                show_message(i18n::trf("“{}” could not be mounted: {}", name, i18n::tr(error)),
                             true);
                return;
            }
            if (ready)
                ready(where);
            if (open) {
                open_mounted_folder(origin, where);
            }
            show_message(i18n::trf("“{}” mounted", name), false);
        });
}

void Browser::open_mounted_folder(const std::shared_ptr<Folder>& origin,
                                  const std::filesystem::path& where) {
    // Completion belongs to the requesting tab even if focus moved during the mount.
    for (std::size_t side = 0; side < areas_.size(); ++side) {
        if (!areas_[side])
            continue;
        AreaContext addressed{context_area_, side};
        for (std::size_t i = 0; i < area().tabs.size(); ++i) {
            AreaContext tab_context{area().active, i};
            if (current_folder() == origin)
                this->open(where);
        }
    }
}

void Browser::open_location() {
    const std::shared_ptr<Folder> shown = current_folder();
    location_.open(shown ? shown->path().string() : std::string{},
                   [this](const std::string& text, bool add) { return go_to(text, add); });
}

std::string Browser::go_to(std::string_view text, bool add) {
    const std::optional<fs::Location> location = fs::parse_location(text, place_sources_.home);
    if (!location) {
        return std::string{
            i18n::tr("Not a folder or a server: start with /, ~, smb:// or sftp://")};
    }
    if (location->uri.empty()) {
        open(location->folder);
        return {};
    }
    if (!remotes_.mount) {
        return std::string{i18n::tr("Servers are mounted through gvfs, which is not available")};
    }
    const std::optional<fs::RemoteUri> remote = fs::parse_remote_uri(location->uri);
    std::string label = remote ? fs::remote_label(*remote) : location->uri;
    if (add) {
        bookmark_changes_.push_back(
            {.add = {}, .remove = std::nullopt, .add_uri = location->uri, .add_label = label});
        next_bookmark_change();
    }
    mount_remote({.section = fs::Place::Section::bookmarks,
                  .label = std::move(label),
                  .path = {},
                  .icon = "server",
                  .disk = {.volume = {}, .mounted = false, .ejectable = false},
                  .uri = location->uri},
                 true);
    return {};
}

void Browser::unmount_remote(const fs::Place& place) {
    if (!remotes_.unmount || place.uri.empty()) {
        return;
    }
    leave_disk(place.path);
    ++operations_;
    remotes_.unmount(place.uri, [this, alive = alive_,
                                 name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(i18n::trf("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(i18n::trf("“{}” could not be unmounted: {}", name, i18n::tr(error)), true);
        } else {
            show_message(i18n::trf("“{}” unmounted", name), false);
        }
    });
}

void Browser::leave_disk(const std::filesystem::path& mount_point) {
    if (mount_point.empty())
        return;
    for (std::size_t side = 0; side < areas_.size(); ++side) {
        if (!areas_[side])
            continue;
        AreaContext addressed{context_area_, side};
        for (std::size_t i = 0; i < area().tabs.size(); ++i) {
            AreaContext tab_context{area().active, i};
            const auto shown = current_folder();
            if (!shown)
                continue;
            const auto inside = shown->path().lexically_relative(mount_point);
            if (!inside.empty() && *inside.begin() != "..") {
                open(place_sources_.home.empty() ? std::filesystem::path{"/"}
                                                 : place_sources_.home);
            }
        }
    }
}

void Browser::unmount_place(const fs::Place& place) {
    if (!devices_.unmount || place.disk.volume.empty()) {
        return;
    }
    leave_disk(place.path);
    ++operations_;
    devices_.unmount(place.disk.volume, [this, alive = alive_,
                                         name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(i18n::trf("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(i18n::trf("“{}” could not be unmounted: {}", name, i18n::tr(error)), true);
        } else {
            show_message(i18n::trf("“{}” unmounted", name), false);
        }
    });
}

void Browser::eject_place(const fs::Place& place) {
    if (!place.uri.empty()) {
        unmount_remote(place);  // a network folder's eject icon
        return;
    }
    if (!devices_.eject || place.disk.volume.empty()) {
        return;
    }
    leave_disk(place.path);
    show_message(i18n::trf("Ejecting “{}”…", place.label), false);
    ++operations_;
    devices_.eject(place.disk.volume, [this, alive = alive_,
                                       name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(i18n::trf("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(i18n::trf("“{}” could not be ejected: {}", name, i18n::tr(error)), true);
        } else {
            show_message(i18n::trf("“{}” can be unplugged now", name), false);
        }
    });
}

void Browser::view_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (pane().insights.is_open()) {
        pane().insights.pointer(event, now);
        return;
    }
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.pointer(event, now);
            break;
        case Mode::grid:
            pane().grid.pointer(event, now);
            break;
        case Mode::details:
            pane().details.pointer(event, now);
            break;
    }
}

std::shared_ptr<Folder> Browser::current_folder() const { return folder_of(pane()); }

bool Browser::key(const ui::KeyEvent& event, Clock::time_point now) {
    clear_control_hint();
    keyboard_focus_visible_ = true;
    redraw_();
    if (batch_rename_preparing_ && event.keysym == XKB_KEY_Escape) {
        ++batch_rename_generation_;
        batch_rename_preparing_ = false;
        batch_rename_.close();
        return true;
    }
    context_area_ = active_area_;
    note_location();  // the cursor of the folder the key may leave
    const bool handled = take_key(event, now);
    note_location();
    return handled;
}

bool Browser::take_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (showing_photos() && event.keysym == XKB_KEY_Escape)
        photo_library_->cancel_album_chooser();
    if (drag_ && event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{}) {
        if (dnd_.cancel) {
            dnd_.cancel();
        }
        drag_ended();
        return true;
    }
    if (password_.is_open()) {
        if (!text_field_key(event))
            password_.key(event);
        return true;
    }
    if (errors_.is_open())
        return errors_.key(event);
    if (conflict_.is_open()) {
        return conflict_.key(event);  // a copy waits for the answer: every key is the card's
    }
    if (confirm_.is_open()) {
        if (text_field_key(event)) {
            return true;
        }
        return confirm_.key(event);
    }
    if (mount_password_.is_open()) {
        if (text_field_key(event)) {
            return true;
        }
        return mount_password_.key(event);  // every key is the card's meanwhile
    }
    if (batch_rename_.is_open()) {
        if (text_field_key(event)) {
            return true;
        }
        return batch_rename_.key(event);
    }
    if (location_.is_open()) {
        if (text_field_key(event)) {
            return true;
        }
        return location_.key(event);
    }
    if (settings_.is_open()) {
        if (event.modifiers == ui::Modifiers{.ctrl = true} &&
            (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V))
            paste_text();
        else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
                 (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C ||
                  event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X)) {
            const auto text =
                settings_.copy_selection(event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X);
            if (!text.empty())
                copy_text(text);
        } else
            settings_.key(event);
        return true;
    }
    if (properties_.is_open()) {
        if (event.modifiers == ui::Modifiers{.ctrl = true} &&
            (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V))
            paste_text();
        else
            properties_.key(event);
        return true;
    }
    if (showing_photos() && photo_library_->has_dialog() && !palette_.is_open() &&
        !menu_.is_open()) {
        if (event.modifiers.ctrl && (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V))
            paste_text();
        else
            photo_library_->key_dialog(event);
        return true;
    }
    if (searching_results() && !palette_.is_open() && !menu_.is_open()) {
        if (quick_look_.is_open()) {
            if (event.keysym == XKB_KEY_Escape || event.keysym == XKB_KEY_space)
                quick_look_.close();
            else if (quick_look_.shows_text() && event.modifiers == ui::Modifiers{.ctrl = true} &&
                     (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A))
                quick_look_.select_all_text();
            else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
                     (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C)) {
                if (auto text = quick_look_.selected_text(); !text.empty())
                    copy_text(std::move(text));
                else
                    search_action(SearchResults::Action::copy);
            } else
                quick_look_.text_key(event, now);
        } else if (event.modifiers.ctrl && (event.keysym == XKB_KEY_f || event.keysym == XKB_KEY_F))
            search_results_->focus_query();
        else if (event.modifiers.ctrl && (event.keysym == XKB_KEY_k || event.keysym == XKB_KEY_K))
            open_palette();
        else if (!search_results_->editing() || !text_field_key(event))
            search_results_->key(event);
        return true;
    }
    found_after_.reset();  // a key while a file found is on its way: it stays shown, no more
    if (palette_.is_open()) {
        if (palette_use_ == PaletteUse::files && finder_ && event.keysym == XKB_KEY_Return &&
            event.modifiers == ui::Modifiers{.ctrl = true}) {
            const auto root = finder_->root();
            const auto query = finder_->query();
            const bool hidden = finder_->hidden();
            open_search_results(root, query, hidden);
            return true;
        }
        const Command command = command_for(event);
        if (command == Command::command_palette || command == Command::find_files ||
            command == Command::select_pattern) {
            // The same key closes it; another switches.
            const PaletteUse was = palette_use_;
            palette_.close();
            if (command == Command::find_files && was != PaletteUse::files) {
                open_finder();
            } else if (command == Command::command_palette && was != PaletteUse::commands) {
                open_palette();
            } else if (command == Command::select_pattern && was != PaletteUse::pattern) {
                open_pattern();
            }
        } else if (!text_field_key(event)) {
            palette_.key(event);  // every key is the palette's meanwhile
        }
        return true;
    }
    if (menu_.is_open()) {
        return menu_.key(event);  // every key is the menu's meanwhile
    }
    if (editor_.active()) {
        if (text_field_key(event)) {
            return true;
        }
        return editor_.key(event);  // every key is the name's meanwhile
    }
    if (sidebar_.renaming()) {
        if (text_field_key(event)) {
            return true;
        }
        return sidebar_.rename_key(event);  // every key is the bookmark's label meanwhile
    }
    const auto focus_command = command_for(event);
    if (focus_command == Command::focus_files || focus_command == Command::focus_places ||
        focus_command == Command::focus_first_pane || focus_command == Command::focus_second_pane) {
        run(focus_command, now);
        return true;
    }
    if (command_for(event) == Command::edit_path) {
        edit_path();
        return true;
    }
    if (area().breadcrumb.editing()) {
        if (!text_field_key(event))
            area().breadcrumb.key(event);
        return true;
    }
    if (filter_focused_ && !shortcuts_.is_open() && !quick_look_.is_open()) {
        if (event.keysym == XKB_KEY_space && event.modifiers == ui::Modifiers{.ctrl = true}) {
            menu_.open(filter_bounds_.x, filter_bounds_.bottom(), name_filter_menu().children,
                       true);
            filter_focused_ = false;
        } else if (command_for(event) == Command::new_folder) {
            new_folder();
        } else if (event.keysym == XKB_KEY_Escape) {
            set_name_filter({});
            blur_filter();
        } else if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
                   event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
            blur_filter();
        } else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
                   (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V)) {
            paste_text();
        } else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
                   (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C ||
                    event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X)) {
            const auto [first, last] = pane().filter_field.selection();
            copy_text(pane().filter_field.text().substr(first, last - first));
            if (event.keysym == XKB_KEY_x || event.keysym == XKB_KEY_X) {
                pane().filter_field.insert({});
                apply_filters();
            }
        } else {
            const auto previous = name_filter();
            pane().filter_field.key(event);
            if (name_filter() != previous) {
                apply_filters();
            }
        }
        redraw_();
        return true;
    }
    const Command command = command_for(event);
    if ((command == Command::none || command == Command::parent_folder) && !sidebar_.focused() &&
        !quick_look_.is_open() && !shortcuts_.is_open() && !type_jump_prefix_.empty() &&
        event.modifiers == ui::Modifiers{} &&
        (event.keysym == XKB_KEY_BackSpace || event.keysym == XKB_KEY_Escape) &&
        type_jump(event, now))
        return true;
    if (command != Command::none)
        clear_type_jump();
    if (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{} &&
        (!cut_marks_.names.empty() || (clipboard_.cut && !clipboard_.paths.empty())) &&
        !shortcuts_.is_open() && !quick_look_.is_open() && !sidebar_.focused()) {
        clipboard_ = {};  // Escape keeps cut entries where they are; the view gets it too
        cut_marks_ = {};
        redraw_();
        view_key(event, now);
        return true;
    }
    if (shortcuts_.is_open()) {
        if (command == Command::show_shortcuts ||
            (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{})) {
            shortcuts_.close();
        } else {
            shortcuts_.key(event);
        }
        return true;  // nothing under them reacts meanwhile
    }
    if (const auto folder = folder_for(event); !folder.empty()) {
        quick_look_.close();
        sidebar_.set_focused(false);
        open(folder);  // The directory model checks availability on a worker and shows errors.
        return true;
    }
    if (command == Command::select_tab) {
        // Alt+1…9: the keysyms 1 to 9 follow each other.
        select_tab(static_cast<std::size_t>(event.keysym - XKB_KEY_1));
        return true;
    }
    if (quick_look_.is_open()) {
        if (command == Command::quick_look ||
            (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{})) {
            quick_look_.close();
            return true;
        }
        // On a text page Home, End, Page Up and Page Down move through it; Ctrl+A selects its
        // text and Ctrl+C copies it (with nothing selected, Ctrl+C copies the file, as without
        // Quick Look).
        if (command >= Command::toggle_split || (command >= Command::scroll_preview_up &&
                                                 command <= Command::scroll_preview_page_down)) {
            return true;  // the info pane is under it, and the panes are not shown
        }
        if (quick_look_.text_key(event, now)) {
            return true;
        }
        if (quick_look_.shows_text()) {
            if ((event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A) &&
                event.modifiers == ui::Modifiers{.ctrl = true}) {
                quick_look_.select_all_text();
                return true;
            }
            if (command == Command::copy) {
                if (std::string text = quick_look_.selected_text(); !text.empty()) {
                    copy_text(std::move(text));
                    return true;
                }
            }
        }
        if (command != Command::none) {
            run(command, now);
            return true;
        }
        // The rest moves the view underneath; Quick Look shows where it lands.
        const bool handled = view_key(event, now);
        quick_look_.follow(current_folder());
        return handled;
    }
    if (!sidebar_.focused()) {
        if (info_pane_.text_key(event, now))
            return true;
        if (const auto id = action_for(event); !id.empty()) {
            run_action(id);
            return true;
        }
    }
    // What is not listed for the sidebar is nothing there (Space: Quick Look shows the view's
    // entries; F2: the sidebar's places are not renamed).
    // F2 and Alt+Up, Alt+Down edit the sidebar's bookmarks: never the view's, off a bookmark too.
    if (sidebar_.focused() && sidebar_.edit_key(event)) {
        return true;
    }
    // entries).
    const bool ignored = sidebar_.focused() && !listed(event, scope::kSidebar);
    if (command != Command::none && !ignored) {
        run(command, now);
        return true;
    }
    if (sidebar_.focused()) {
        // The view does not see keys meanwhile.
        return listed(event, scope::kSidebar) && sidebar_.key(view_key_event(event));
    }
    const auto before = current_folder();
    const auto revision = before ? before->selection_revision() : 0;
    const bool handled = view_key(event, now);
    if (current_folder() != before || (before && before->selection_revision() != revision))
        info_pane_.clear_text_focus();
    return handled;
}

void Browser::run(Command command, Clock::time_point now) {
    if (batch_rename_preparing_ || batch_rename_running_) {
        return;
    }
    if (showing_photos() &&
        (command == Command::same_other_pane || command == Command::add_bookmark ||
         command == Command::reset_folder_view))
        return;
    if (showing_photos() && !sidebar_.focused()) {
        switch (command) {
            case Command::parent_folder:
            case Command::collapse_folder:
                photo_library_->show_all_photos();
                return;
            case Command::folder_insights:
                open_photo_menu(insights_bounds_.x, insights_bounds_.bottom(), true);
                return;
            case Command::find_files:
                filter_focused_ = true;
                pane().filter_field.select_all();
                redraw_();
                return;
            case Command::open_selected_tab:
            case Command::expand_folder:
                open_selection();
                return;
            case Command::quick_look:
                if (!photo_library_->photo_rows()) {
                    photo_library_->activate();
                    return;
                }
                break;
            case Command::add_bookmark:
            case Command::reset_folder_view:
            case Command::same_other_pane:
            case Command::open_other_pane:
                return;
            default:
                break;
        }
    }
    if (command < Command::scroll_preview_up || command > Command::scroll_preview_page_down)
        info_pane_.clear_text_focus();
    if (!panes_enabled_ && command >= Command::toggle_split)
        return;
    if (sidebar_.focused()) {
        std::string_view id;
        switch (command) {
            case Command::open_selected:
                id = "open";
                break;
            case Command::open_selected_tab:
                if (!panes_enabled_ || picker_mode_)
                    return;
                id = "open_tab";
                break;
            case Command::open_other_pane:
                id = "open_other_pane";
                break;
            case Command::open_terminal:
                id = "open_terminal";
                break;
            case Command::copy_path:
                id = "copy_path";
                break;
            case Command::properties:
                id = "properties";
                break;
            default:
                break;
        }
        if (!id.empty()) {
            if (const auto* place = sidebar_.cursor_place()) {
                if (command == Command::properties &&
                    place->section == fs::Place::Section::devices && !Sidebar::child(*place)) {
                    if (place->disk.mounted && !place->path.empty())
                        open_properties(place->path, false, true, place->label);
                } else {
                    const auto items = place_navigation_items(*place);
                    const auto item = std::ranges::find(items, id, &MenuItem::id);
                    if (item != items.end() && item->enabled && item->action)
                        item->action();
                }
            }
            return;
        }
    }
    if (const std::shared_ptr<Folder> shown = current_folder(); shown && shown->is_trash()) {
        switch (command) {
            case Command::rename:
            case Command::new_folder:
            case Command::copy:
            case Command::cut:
            case Command::paste:
                show_message(
                    std::string{i18n::tr("The trash only gives back: right click, Restore")},
                    false);
                return;
            case Command::trash:
                erase();  // from the trash, only for good
                return;
            default:
                break;
        }
    }
    if (const auto shown = current_folder(); shown && shown->is_archive()) {
        switch (command) {
            case Command::rename:
            case Command::new_folder:
            case Command::cut:
            case Command::paste:
            case Command::trash:
            case Command::erase:
            case Command::move_other_pane:
                show_message(std::string{i18n::tr(
                                 "Archive contents are read-only; extract the archive first")},
                             false);
                return;
            default:
                break;
        }
    }
    switch (command) {
        case Command::edit_path:
            edit_path();
            break;
        case Command::focus_files:
            focus_files();
            break;
        case Command::focus_places:
            focus_files();
            set_sidebar_visible(true);
            sidebar_.set_focused(true);
            break;
        case Command::focus_first_pane:
            focus_files();
            focus_pane(0);
            break;
        case Command::focus_second_pane:
            focus_files();
            focus_pane(1);
            break;
        case Command::open_selected:
            browse_key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {}}, now);
            break;
        case Command::open_selected_tab:
            open_selected_tab();
            break;
        case Command::parent_folder:
            browse_key({.keysym = XKB_KEY_Up, .text = {}, .modifiers = {.alt = true}}, now);
            break;
        case Command::expand_folder:
            browse_key({.keysym = static_cast<xkb_keysym_t>(
                            pane().mode == Mode::grid ? XKB_KEY_Return : XKB_KEY_Right),
                        .text = {},
                        .modifiers = {}},
                       now);
            break;
        case Command::collapse_folder:
            browse_key({.keysym = static_cast<xkb_keysym_t>(
                            pane().mode == Mode::grid ? XKB_KEY_BackSpace : XKB_KEY_Left),
                        .text = {},
                        .modifiers = {}},
                       now);
            break;
        case Command::show_settings:
            open_settings();
            break;
        case Command::toggle_split:
            toggle_split();
            break;
        case Command::switch_pane:
            switch_pane();
            break;
        case Command::swap_panes:
            swap_panes();
            break;
        case Command::orient_panes:
            set_stacked(!stacked_);
            break;
        case Command::copy_other_pane:
            transfer_to_other_pane(false);
            break;
        case Command::move_other_pane:
            transfer_to_other_pane(true);
            break;
        case Command::same_other_pane:
            if (const auto folder =
                    pane().mode == Mode::details ? pane().details.root_folder() : current_folder())
                open_in_other_pane(folder->path());
            break;
        case Command::open_other_pane:
            if (const auto folder = current_folder()) {
                const auto entry = folder->single_selection();
                if (entry && entry->is_directory())
                    open_in_other_pane(folder->path() / entry->name);
            }
            break;

        case Command::filter_names:
            quick_look_.close();
            sidebar_.set_focused(false);
            filter_focused_ = true;
            pane().filter_field.select_all();
            redraw_();
            break;
        case Command::none:
            break;
        case Command::columns_view:
            set_mode(Mode::columns);
            break;
        case Command::grid_view:
            set_mode(Mode::grid);
            break;
        case Command::details_view:
            set_mode(Mode::details);
            break;
        case Command::toggle_info:
            set_info_visible(!info_shown());
            break;
        case Command::scroll_preview_up:
            info_pane_.scroll_text(InfoPane::TextScroll::line_up, now);
            break;
        case Command::scroll_preview_down:
            info_pane_.scroll_text(InfoPane::TextScroll::line_down, now);
            break;
        case Command::scroll_preview_page_up:
            info_pane_.scroll_text(InfoPane::TextScroll::page_up, now);
            break;
        case Command::scroll_preview_page_down:
            info_pane_.scroll_text(InfoPane::TextScroll::page_down, now);
            break;
        case Command::toggle_sidebar:
            set_sidebar_visible(!sidebar_visible_);
            break;
        case Command::focus_sidebar:
            if (!sidebar_.focused()) {
                set_sidebar_visible(true);
            }
            sidebar_.set_focused(!sidebar_.focused());
            break;
        case Command::quick_look:
            if (quick_look_.is_open()) {
                quick_look_.close();
            } else {
                quick_look_.open(current_folder(), now);
            }
            break;
        case Command::show_shortcuts:
            shortcuts_.open();
            break;
        case Command::rename:
            quick_look_.close();
            start_rename();
            break;
        case Command::new_folder:
            quick_look_.close();
            new_folder();
            break;
        case Command::copy:
            take_selection(false);
            break;
        case Command::copy_path:
            copy_path();
            break;
        case Command::cut:
            take_selection(true);
            break;
        case Command::paste:
            quick_look_.close();
            paste();
            break;
        case Command::trash:
            trash();
            break;
        case Command::erase:
            erase();
            break;
        case Command::undo:
            undo();
            break;
        case Command::redo:
            redo();
            break;
        case Command::stop_transfer:
            if (transfers_.pending() > 0) {
                cancel_transfer();
            } else {
                show_message(std::string{i18n::tr("Nothing is being copied, moved or deleted")},
                             false);
            }
            break;
        case Command::context_menu:
            open_menu_from_keyboard();
            break;
        case Command::folder_insights:
            if (pane().insights.is_open()) {
                filter_focused_ = false;
                menu_.open(insights_bounds_.x, insights_bounds_.bottom(), insights_menu(), true);
            } else
                show_insights();
            break;
        case Command::properties:
            show_properties();
            break;
        case Command::back:
            back();
            break;
        case Command::forward:
            forward();
            break;
        case Command::toggle_thumbnails:
            set_show_thumbnails(!shows_thumbnails());
            break;
        case Command::toggle_extensions:
            set_show_extensions(!shows_extensions());
            break;
        case Command::toggle_hidden:
            set_show_hidden(!pane().show_hidden);
            break;
        case Command::new_tab:
            new_tab();
            break;
        case Command::new_window:
            if (!picker_mode_ && new_window_) {
                const auto folder = current_folder();
                new_window_(folder && !folder->is_collection() ? folder->path() : home_directory());
            }
            break;
        case Command::close_tab:
            close_tab(area().active);
            break;
        case Command::reopen_tab:
            reopen_tab();
            break;
        case Command::next_tab:
            select_tab((area().active + 1) % area().tabs.size());
            break;
        case Command::previous_tab:
            select_tab((area().active + area().tabs.size() - 1) % area().tabs.size());
            break;
        case Command::select_tab:
            break;  // take_key: it needs the digit
        case Command::add_bookmark:
            if (const std::shared_ptr<Folder> shown = current_folder()) {
                add_to_sidebar({shown->path()});
            }
            break;
        case Command::open_terminal:
            open_terminal();
            break;
        case Command::command_palette:
            open_palette();
            break;
        case Command::reset_folder_view:
            reset_folder_view();
            break;
        case Command::select_pattern:
            open_pattern();
            break;
        case Command::find_files:
            open_finder();
            break;
        case Command::go_to:
            open_location();
            break;
    }
}

bool Browser::opens_as_folder(const fs::Entry& file) const {
    const auto type = file.mime != fs::kUnknownMime ? file.mime : mime_->by_name(file.name).mime;
    const auto* family = engines::installed()->find(*mime_, type);
    return family != nullptr && family->shows == engines::Shows::archive;
}

bool Browser::open_selection() {
    if (showing_photos()) {
        if (photo_library_->activate())
            return true;
        const auto folder = photo_library_->folder();
        const auto* entry = folder->cursor_entry();
        if (entry && photo_library_->photo_rows()) {
            if (entry->can_preview())
                quick_look_.open(folder, Clock::now());
            else
                show_message(
                    "The original is offline; its cached information is available in the preview "
                    "pane",
                    false);
        }
        return true;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    if (cursor == nullptr || cursor->is_directory()) {
        return false;
    }
    if (folder->is_trash()) {
        show_message(std::string{i18n::tr("Restore it first to open it")}, false);
        return true;
    }
    if (!picker_mode_ && (!folder->is_selected(cursor->key()) || folder->selected_count() == 1)) {
        fs::Entry typed = *cursor;
        typed.mime = folder->mime_of(*cursor);
        if (opens_as_folder(typed)) {
            open(folder->path() / cursor->name);
            return true;
        }
    }
    if (!open_) {
        return false;
    }
    const auto with_type = [&folder](fs::Entry entry) {
        entry.mime = folder->mime_of(entry);
        return entry;
    };
    std::vector<fs::Entry> files;
    if (!folder->is_selected(cursor->key())) {
        files.push_back(with_type(*cursor));
    } else {
        for (const fs::Entry& entry : folder->entries()) {
            if (!entry.is_directory() && folder->is_selected(entry.key())) {
                files.push_back(with_type(entry));
            }
        }
    }
    if (folder->is_archive())
        open_archive_members(folder->path(), std::move(files));
    else
        open_(folder->path(), std::move(files));
    return true;
}

void Browser::open_terminal() {
    if (showing_photos()) {
        const auto row = photo_library_->focused_photo();
        if (terminal_ && row.value("availability", "") == "online")
            terminal_(std::filesystem::path{row.at("path").get<std::string>()}.parent_path());
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || !terminal_) {
        return;
    }
    if (folder->is_trash()) {
        show_message(std::string{i18n::tr("Restore it first to open it")}, false);
        return;
    }
    if (folder->is_archive()) {
        show_message(std::string{i18n::tr("A terminal cannot open inside an archive")}, false);
        return;
    }
    const fs::Entry* const cursor = folder->cursor_entry();
    terminal_(cursor != nullptr && cursor->is_directory() ? folder->path() / cursor->name
                                                          : folder->path());
}

bool Browser::view_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (pane().insights.is_open()) {
        pane().insights.key(event);
        return true;
    }
    const auto scope = pane().mode == Mode::columns ? scope::kColumns
                       : pane().mode == Mode::grid  ? scope::kGrid
                                                    : scope::kDetails;
    if (listed(event, scope)) {
        clear_type_jump();
        return browse_key(view_key_event(event), now);
    }
    if (keyboard_config_.vim && !event.modifiers.alt && !event.modifiers.super &&
        (event.text.empty() || event.text.size() == 1)) {
        auto mapped = event;
        switch (event.keysym) {
            case XKB_KEY_h:
            case XKB_KEY_H:
                mapped.keysym = XKB_KEY_Left;
                break;
            case XKB_KEY_j:
            case XKB_KEY_J:
                mapped.keysym = XKB_KEY_Down;
                break;
            case XKB_KEY_k:
            case XKB_KEY_K:
                mapped.keysym = XKB_KEY_Up;
                break;
            case XKB_KEY_l:
            case XKB_KEY_L:
                mapped.keysym = XKB_KEY_Right;
                break;
            default:
                mapped.keysym = XKB_KEY_NoSymbol;
                break;
        }
        if (mapped.keysym != XKB_KEY_NoSymbol) {
            clear_type_jump();
            return browse_key(mapped, now);
        }
    }
    return !quick_look_.is_open() && type_jump(event, now);
}

bool Browser::browse_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (pane().insights.is_open()) {
        pane().insights.key(event);
        return true;
    }
    if ((event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter) &&
        event.modifiers == ui::Modifiers{} && open_selection()) {
        return true;  // a file: its application; a folder goes on to the view
    }
    pane().columns.set_wrap_navigation(keyboard_config_.wrap);
    pane().grid.set_wrap_navigation(keyboard_config_.wrap);
    pane().details.set_wrap_navigation(keyboard_config_.wrap);
    // Only what the shortcuts list reaches the view.
    switch (pane().mode) {
        case Mode::columns:
            return pane().columns.key(event, now);
        case Mode::grid:
            return pane().grid.key(event, now);
        case Mode::details:
            return pane().details.key(event, now);
    }
    return false;
}

}  // namespace rummage::views
