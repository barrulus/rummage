#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/column_view.hpp"
#include "rummage/views/details_view.hpp"
#include "rummage/views/grid_view.hpp"
#include "rummage/views/info_pane.hpp"
#include "rummage/views/insights_view.hpp"
#include "rummage/views/search_results.hpp"
#include "rummage/views/state.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// The three ways of showing a folder.
enum class ViewMode : std::uint8_t { columns, grid, details };

// A folder opened, as the history keeps it: where the cursor was when it was left.
struct HistoryStep {
    std::filesystem::path folder;
    std::string cursor;
    std::optional<SearchResults::Snapshot> search{};
};

// One tab's browsing state. A Browser owns two independent tab groups, with one visible
// tab in each while split. Folder models, history, filters and view settings belong here;
// the browser shares its sidebar, preview, dialogs, thumbnail cache, transfers and undo.
struct Pane {
    Pane(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
         const std::shared_ptr<const fs::MimeDatabase>& mime, const std::function<void()>& redraw,
         const std::function<void(ui::CursorShape)>& set_cursor, Thumbnails* thumbnails,
         InfoPane* info_pane);

    std::unique_ptr<SearchResults> search_results;
    ColumnView columns;  // before grid: grid folders may report to it
    GridView grid;
    DetailsView details;
    InsightsView insights;
    ui::TextField filter_field;
    std::string valid_filter;
    std::string filter_error;
    fs::Listing::Predicate entry_filter;
    bool show_hidden{false};
    // Collection destinations share the real browser views and frame. They have no
    // filesystem root and must not be passed to directory mutation operations.
    bool photos{false};
    bool photo_records{false};
    std::vector<fs::SortKey> photo_columns;
    ViewMode mode{ViewMode::columns};
    std::filesystem::path settings_folder;  // whose preferences are applied
    std::optional<State::FolderSettings> remembered_settings;
    // Column focus may retain a saved mode without applying it to the current column chain.
    std::optional<State::Mode> retained_folder_mode;
    bool restore_mode_on_arrival{false};  // explicit navigation can finish unfolding asynchronously
    std::vector<HistoryStep> history;
    std::size_t here{0};  // where history is now
};

}  // namespace rummage::views
