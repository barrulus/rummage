#include <algorithm>
#include <format>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {

void Browser::start_batch_rename(const std::shared_ptr<Folder>& folder,
                                 std::vector<std::string> names) {
    if (picker_mode_ || folder->is_trash() || (folder->is_archive() || folder->is_collection()) ||
        batch_rename_.is_open() || batch_rename_preparing_ || batch_rename_running_) {
        return;
    }
    if (operations_ > 0 || transfers_.pending() > 0 || undoing_) {
        show_message(std::string{i18n::tr("Wait for the current operation before batch renaming")},
                     false);
        return;
    }
    batch_rename_preparing_ = true;
    const auto generation = ++batch_rename_generation_;
    batch_rename_.open_busy(std::string{i18n::tr("Preparing Batch Rename… (Escape cancels)")});
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path()](const core::CancelToken&) {
            return fs::rename_directory(directory);
        },
        [this, alive = alive_, folder, generation,
         names = std::move(names)](fs::RenameDirectory snapshot) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (generation != batch_rename_generation_) {
                return;
            }
            batch_rename_preparing_ = false;
            batch_rename_.close();
            if (!snapshot.error.empty()) {
                show_message(i18n::trf("Cannot prepare Batch Rename: {}", snapshot.error), true);
                return;
            }
            if (current_folder() != folder || picker_mode_ || batch_rename_.is_open()) {
                return;
            }
            batch_rename_.open(std::move(snapshot.entries), names,
                               [this, directory = folder->path()](const auto& changes) {
                                   apply_batch_rename(directory, changes, Purpose::user);
                               });
        });
}

void Browser::apply_batch_rename(const std::filesystem::path& directory,
                                 std::vector<fs::RenameChange> changes, Purpose purpose) {
    if (batch_rename_preparing_ || batch_rename_running_ || operations_ > 0 ||
        transfers_.pending() > 0 || undoing_) {
        show_message(std::string{i18n::tr("Wait for the current operation before batch renaming")},
                     false);
        return;
    }
    batch_rename_running_ = true;
    undoing_ = true;
    batch_rename_.open_busy(
        std::string{i18n::tr(purpose == Purpose::user ? "Renaming files…" : "Restoring names…")});
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory, changes = std::move(changes)](const core::CancelToken&) {
            return fs::rename_batch(directory, changes);
        },
        [this, alive = alive_, directory, purpose](const fs::BatchRenamed& result) {
            if (!*alive) {
                return;
            }
            --operations_;
            batch_rename_running_ = false;
            undoing_ = false;
            batch_rename_.close();
            if (!result.error.empty()) {
                undoing_ = false;
                std::string detail = result.error;
                for (const auto& path : result.recovery) {
                    detail += "\n" + path;
                }
                errors_.push(std::move(detail));
                show_message(std::string{i18n::tr("Batch Rename failed; see details")}, true);
                return;
            }
            FileOperation operation{
                .kind = FileOperation::Kind::batch_rename, .items = {}, .renames = result.changed};
            for (const auto& change : result.changed) {
                operation.items.push_back(
                    {.from = directory / change.from, .to = directory / change.to});
            }
            record(std::move(operation), purpose, false);
            if (purpose == Purpose::user) {
                show_message(
                    i18n::trn("Renamed {} item, including sidecars",
                              "Renamed {} items, including sidecars", result.changed.size()),
                    false);
            }
        });
}

}  // namespace rummage::views
