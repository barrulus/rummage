#include <algorithm>
#include <map>
#include <utility>

#include "rummage/views/browser.hpp"

namespace rummage::views {
namespace {
using Json = nlohmann::json;

fs::SourceIdentity identity(const Json& photo) {
    return {.path = photo.at("path").get<std::string>(),
            .device = photo.at("device").get<std::uint64_t>(),
            .inode = photo.at("inode").get<std::uint64_t>(),
            .size = photo.at("size").get<std::uint64_t>(),
            .mtime_ns = photo.at("mtime_ns").get<std::int64_t>()};
}
}  // namespace

void Browser::photo_operation(const Json& input) {
    if (!photos_enabled_ || picker_mode_ || !photo_library_ || batch_rename_running_ ||
        batch_rename_preparing_)
        return;
    const auto client = photo_library_->client();
    const auto kind = input.value("kind", "");
    if (kind != "trash" && kind != "restore")
        return;
    Json preparation = input;
    preparation["op"] = kind == "trash" ? "operation_prepare" : "operation_restore_prepare";
    photo_library_->report("Checking the selected originals…");
    pool_.submit(
        core::Priority::visible,
        [client, preparation](const core::CancelToken&) { return client->request(preparation); },
        [this, alive = alive_, client, kind](Json prepared) {
            if (!*alive)
                return;
            if (!prepared.value("ok", false)) {
                show_message(prepared.value("error", "Photo operation could not be prepared"),
                             false);
                if (photo_library_)
                    photo_library_->report(prepared.value("error", "Photo operation failed"));
                return;
            }
            if (!photos_enabled_ || !photo_library_ || batch_rename_running_ ||
                batch_rename_preparing_)
                return;  // Durable intent remains reviewable; never dispatch after disabling.
            std::map<std::filesystem::path, Json> folders;
            for (const auto& photo : prepared.at("photos")) {
                const std::filesystem::path path{photo.at("path").get<std::string>()};
                auto [it, inserted] = folders.try_emplace(path.parent_path(), Json::array());
                static_cast<void>(inserted);
                it->second.push_back(photo);
            }
            const auto batch =
                open_batch(Purpose::user, kind == "trash" ? FileOperation::Kind::trash
                                                          : FileOperation::Kind::restore);
            for (const auto& [folder, photos] : folders) {
                fs::TransferRequest transfer;
                transfer.kind =
                    kind == "trash" ? fs::TransferKind::trash : fs::TransferKind::restore;
                transfer.source = folder;
                transfer.destination = home_trash_;
                for (const auto& photo : photos) {
                    const std::filesystem::path path{photo.at("path").get<std::string>()};
                    // Restore consumes full trash paths, as in Browser::restore_selection.
                    transfer.names.push_back(kind == "trash" ? path.filename().string()
                                                             : path.string());
                    transfer.source_identities.push_back(identity(photo));
                }
                for (const auto& keeper : prepared.value("keepers", Json::array()))
                    transfer.retained_identities.push_back(identity(keeper));
                submit(std::move(transfer), batch);
                photo_operations_.emplace(
                    submitted_.back().id,
                    PhotoOperation{client, prepared.at("operation_id").get<std::int64_t>(),
                                   photos});
            }
            photo_library_->report(
                "File operation queued; the catalog will update after completion.");
        });
}

void Browser::photo_transfer_done(const fs::TransferRequest& request,
                                  const fs::TransferResult& result) {
    const auto found = photo_operations_.find(request.id);
    if (found == photo_operations_.end())
        return;
    auto operation = std::move(found->second);
    photo_operations_.erase(found);
    Json outcomes = Json::array();
    for (const auto& photo : operation.photos) {
        const std::filesystem::path original{photo.at("path").get<std::string>()};
        const auto placed = std::ranges::find(result.items, original, &fs::PlacedItem::from);
        Json outcome{{"id", photo.at("id")}, {"success", placed != result.items.end()}};
        if (placed != result.items.end())
            outcome["destination"] = placed->to.string();
        else
            outcome["error"] = !result.refused.empty()  ? result.refused
                               : !result.errors.empty() ? result.errors.front()
                               : result.cancelled       ? "Cancelled before this file was moved"
                                                        : "File was not moved";
        outcomes.push_back(std::move(outcome));
    }
    pool_.submit(
        core::Priority::visible,
        [client = operation.client, id = operation.operation_id,
         outcomes](const core::CancelToken&) {
            return client->request(
                {{"op", "operation_complete"}, {"operation_id", id}, {"results", outcomes}});
        },
        [this, alive = alive_](Json saved) {
            if (!*alive)
                return;
            if (!saved.value("ok", false)) {
                const auto error = "Files were processed, but the photo catalog needs recovery: " +
                                   saved.value("error", "catalog update failed");
                show_message(error, false);
                if (photo_library_)
                    photo_library_->report(error);
            } else if (photo_library_) {
                photo_library_->refresh();
            }
        });
}

void Browser::photo_reconcile(const FileOperation& operation) {
    if (!photos_enabled_ || picker_mode_ || operation.items.empty())
        return;
    std::string kind;
    switch (operation.kind) {
        case FileOperation::Kind::rename:
        case FileOperation::Kind::batch_rename:
            kind = "rename";
            break;
        case FileOperation::Kind::move:
        case FileOperation::Kind::group:
            kind = "move";
            break;
        case FileOperation::Kind::trash:
            kind = "trash";
            break;
        case FileOperation::Kind::restore:
            kind = "restore";
            break;
        default:
            return;
    }
    Json items = Json::array();
    for (const auto& item : operation.items)
        items.push_back({{"from", item.from.string()}, {"to", item.to.string()}});
    pool_.submit(
        core::Priority::visible,
        [kind, items](const core::CancelToken&) -> Json {
            // Ordinary browsing must neither create a catalog nor load the optional component
            // for a user who has never opened Photo Library.
            std::error_code error;
            const auto catalog = photos::Client::default_catalog_path();
            if (catalog.empty() || !std::filesystem::is_regular_file(catalog, error) ||
                photos::Client::installed_path().empty())
                return {{"ok", true}};
            return photos::Client{}.request(
                {{"op", "reconcile_locations"}, {"kind", kind}, {"items", items}});
        },
        [this, alive = alive_](Json saved) {
            if (!*alive)
                return;
            if (!saved.value("ok", false))
                show_message("Photo location update needs review: " +
                                 saved.value("error", "catalog update failed"),
                             false);
            else if (!saved.value("review", Json::array()).empty())
                show_message(std::to_string(saved.at("review").size()) +
                                 " photo locations need review in Photo Library Activity.",
                             false);
            if (photo_library_)
                photo_library_->refresh();
        });
}
}  // namespace rummage::views
