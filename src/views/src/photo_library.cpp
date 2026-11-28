#include "rummage/views/photo_library.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/file.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/views/dialog_chrome.hpp"

namespace rummage::views {
namespace {
using Json = nlohmann::json;
using Id = PhotoLibrary::Id;

std::string tr(std::string_view text) { return std::string{i18n::tr(text)}; }

std::string photo_label(std::string_view value) {
    for (const auto& [code, label] :
         std::array{std::pair{"online", i18n::mark("Online")},
                    std::pair{"offline", i18n::mark("Offline")},
                    std::pair{"inactive", i18n::mark("Inactive")},
                    std::pair{"missing", i18n::mark("Missing")},
                    std::pair{"unreadable", i18n::mark("Unreadable")},
                    std::pair{"changed", i18n::mark("Changed")},
                    std::pair{"replaced", i18n::mark("Replaced")},
                    std::pair{"trashed", i18n::mark("In Trash")},
                    std::pair{"running", i18n::mark("Running")},
                    std::pair{"queued", i18n::mark("Queued")},
                    std::pair{"paused", i18n::mark("Paused")},
                    std::pair{"waiting", i18n::mark("Waiting")},
                    std::pair{"failed", i18n::mark("Failed")},
                    std::pair{"completed", i18n::mark("Completed")},
                    std::pair{"cancelled", i18n::mark("Cancelled")},
                    std::pair{"pending", i18n::mark("Pending")},
                    std::pair{"scan", i18n::mark("Index photos")},
                    std::pair{"duplicates", i18n::mark("Find exact duplicates")},
                    std::pair{"trash", i18n::mark("Move to Trash")},
                    std::pair{"restore", i18n::mark("Restore")},
                    std::pair{"discovering", i18n::mark("Finding photos")},
                    std::pair{"metadata", i18n::mark("Reading metadata")},
                    std::pair{"finishing", i18n::mark("Finishing")},
                    std::pair{"manual", i18n::mark("Manual")},
                    std::pair{"exif", i18n::mark("EXIF metadata")},
                    std::pair{"unknown", i18n::mark("Unknown")}})
        if (value == code)
            return tr(label);
    return std::string{value};
}

Json sort_filter(const Json& filter) {
    Json result = Json::object();
    for (const auto* key : {"sort", "descending"})
        if (filter.contains(key))
            result[key] = filter[key];
    return result;
}

std::vector<std::string> split(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto found = value.find(',', start);
        const auto end = found == std::string_view::npos ? value.size() : found;
        const auto part = value.substr(start, end - start);
        const auto first = part.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos)
            result.emplace_back(part.substr(first, part.find_last_not_of(" \t\r\n") - first + 1));
        start = end + 1;
    }
    return result;
}

std::string text_value(const Json& row, std::string_view field) {
    const auto found = row.find(std::string{field});
    if (found == row.end() || found->is_null())
        return {};
    if (found->is_string())
        return found->get<std::string>();
    if (!found->is_array())
        return found->dump();
    std::string text;
    for (const auto& item : *found) {
        if (!text.empty())
            text += ", ";
        text += item.is_string() ? item.get<std::string>() : item.value("name", "");
    }
    return text;
}

Id photo_id(std::string_view key) {
    if (!key.starts_with("photo:"))
        return 0;
    key.remove_prefix(6);
    Id id{};
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size() ? id : 0;
}

bool number(std::string_view text, int& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
}  // namespace

PhotoLibrary::PhotoLibrary(core::EventLoop& loop, core::WorkerPool& pool,
                           fs::DirectoryWatcher& watcher,
                           std::shared_ptr<const fs::MimeDatabase> mime,
                           std::function<void()> redraw)
    : loop_{loop},
      pool_{pool},
      watcher_{watcher},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      client_{std::make_shared<photos::Client>()},
      folder_{std::make_shared<Folder>(loop, pool, watcher, mime_, redraw_)},
      confirm_{redraw_} {
    folder_->set_visible_range_handler([this, alive = alive_](std::size_t, std::size_t last) {
        if (*alive && last > 0 && last + 40 >= rows_.size())
            load_more();
    });
}

PhotoLibrary::~PhotoLibrary() {
    *alive_ = false;
    source_watcher_.reset();
    loop_.remove(work_timer_);
    loop_.remove(metadata_retry_timer_);
    query_job_.cancel();
    for (const auto& [id, token] : pending_)
        token.cancel();
}

void PhotoLibrary::changed() {
    state_dirty_ = true;
    if (changed_)
        changed_();
    redraw_();
}

void PhotoLibrary::report(std::string message) {
    message_ = std::move(message);
    changed();
}

bool PhotoLibrary::loading() const { return querying_ || working_ || !pending_.empty(); }

bool PhotoLibrary::photo_rows() const {
    return destination_ == Destination::library || destination_ == Destination::duplicate_group;
}

std::string PhotoLibrary::title() const {
    switch (destination_) {
        case Destination::library:
            if (active_saved_search_ > 0)
                return collection_name_.empty() ? tr("Saved Photo Search")
                                                : tr("Search") + ": " + collection_name_;
            if (filter_.contains("album_id"))
                return collection_name_.empty() ? tr("Photo Album")
                                                : tr("Album") + ": " + collection_name_;
            if (filter_.contains("source_id"))
                return collection_name_.empty() ? tr("Photos in Folder") : collection_name_;
            return tr("Photo Library");
        case Destination::sources:
            return tr("Photo Sources");
        case Destination::albums:
            return tr("Photo Albums");
        case Destination::filters:
            return tr("Saved Photo Searches");
        case Destination::review:
            return tr("Review Duplicates");
        case Destination::duplicate_group:
            return tr("Duplicate Candidates");
        case Destination::activity:
            return tr("Photo Activity");
    }
    return tr("Photo Library");
}

std::string PhotoLibrary::status() const {
    sync_selection();
    std::string text;
    if (destination_ == Destination::duplicate_group)
        text = i18n::trf("{} candidates · {} kept · {} marked for trash", rows_.size(),
                         keepers_.size(), rows_.size() - keepers_.size());
    else if (destination_ == Destination::library)
        text = i18n::trf("{} photos · {} selected · {} loaded", total_, selected_.size(),
                         rows_.size());
    else
        text = i18n::trn("{} item", "{} items", records_.size());
    if (destination_ == Destination::review || destination_ == Destination::duplicate_group)
        text += " · " + tr("All active online sources");
    if (destination_ == Destination::review)
        text += " · " + i18n::trf("Groups {}–{} of {}", records_.empty() ? 0 : review_offset_ + 1,
                                  review_offset_ + records_.size(), review_total_);
    if (filter_.value("retained_only", false) && destination_ == Destination::library)
        text += " · " + tr("Retained associations: inactive sources and replaced originals");
    if (querying_)
        text += " · " + tr("Updating…");
    if (metadata_refresh_pending_)
        text += " · " + tr("Capture-date results will update as metadata arrives");
    if (background_paused_)
        text += " · " + tr("Background paused");
    if (!message_.empty())
        text += " · " + message_;
    if (!progress_.empty())
        text += " · " + progress_;
    return text;
}

void PhotoLibrary::sync_selection() const {
    if (!catalog_identity_ready_ || publishing_ ||
        selection_revision_ == folder_->selection_revision())
        return;
    selection_revision_ = folder_->selection_revision();
    if (!photo_rows())
        return;
    // Native selection is authoritative after a user gesture. In particular, plain clicks
    // and keyboard movement replace every bulk target, including any no-longer-visible IDs.
    // Programmatic refresh/append publication records its revision separately below.
    selected_.clear();
    restored_selection_pending_ = false;
    for (const auto& key : folder_->selected_names())
        if (const auto id = photo_id(key); id > 0)
            selected_.insert(id);
    std::erase_if(selected_snapshots_,
                  [this](const auto& item) { return !selected_.contains(item.first); });
    for (const auto& row : rows_) {
        const auto id = row.value("id", Id{});
        if (selected_.contains(id) && !selected_snapshots_.contains(id))
            selected_snapshots_.emplace(id, row);
    }
    if (const auto* entry = folder_->cursor_entry())
        cursor_ = photo_id(entry->key());
    else
        cursor_ = 0;
    state_dirty_ = true;
}

void PhotoLibrary::clear_photo_selection() {
    publishing_ = true;
    folder_->set_cursor(std::nullopt);
    publishing_ = false;
    selected_.clear();
    selected_snapshots_.clear();
    restored_selection_pending_ = false;
    focused_properties_.reset();
    selected_properties_.reset();
    cursor_ = 0;
    selection_revision_ = folder_->selection_revision();
    state_dirty_ = true;
}

const std::set<Id>& PhotoLibrary::selection() const {
    sync_selection();
    return selected_;
}

PhotoLibrary::Json PhotoLibrary::selected_ids() const {
    if (!catalog_identity_ready_)
        return Json::array();
    sync_selection();
    Json ids = Json::array();
    for (Id id : selected_)
        ids.push_back(id);
    return ids;
}

PhotoLibrary::Json PhotoLibrary::selected_photos() const {
    if (!catalog_identity_ready_)
        return Json::array();
    sync_selection();
    Json result = Json::array();
    for (Id id : selected_) {
        const auto found = selected_snapshots_.find(id);
        if (found != selected_snapshots_.end())
            result.push_back(found->second);
    }
    return result;
}

PhotoLibrary::Json PhotoLibrary::focused_photo() const {
    const auto* entry = folder_->cursor_entry();
    if (!entry || !photo_rows())
        return Json::object();
    const auto id = photo_id(entry->key());
    const auto found = row_by_id_.find(id);
    return found == row_by_id_.end() ? Json::object() : rows_[found->second];
}

PhotoLibrary::Json PhotoLibrary::focused_record() const {
    const auto* entry = folder_->cursor_entry();
    if (!entry)
        return Json::object();
    const auto found = record_by_key_.find(entry->key());
    return found == record_by_key_.end() ? Json::object() : found->second;
}

const std::string& PhotoLibrary::state() const {
    sync_selection();
    if (state_dirty_) {
        cached_state_ =
            Json{{"version", 3},
                 {"catalog_identity",
                  restored_catalog_state_ ? restored_catalog_identity_ : catalog_identity_},
                 {"text", search_},
                 {"filter", filter_},
                 {"selected", selected_},
                 {"cursor", cursor_},
                 {"offset", 0},
                 {"loaded", offset_},
                 {"saved_search_id", active_saved_search_},
                 {"collection_name", collection_name_}}
                .dump();
        state_dirty_ = false;
    }
    return cached_state_;
}

void PhotoLibrary::restore_state(std::string_view value) {
    if (value.empty() || value.size() > 4 * 1024 * 1024)
        return;
    try {
        const auto saved = Json::parse(value);
        if (saved.value("version", 0) < 1 || saved.value("version", 0) > 3)
            return;
        auto filter = saved.value("filter", Json::object());
        auto text = saved.value("text", std::string{});
        auto ids = saved.value("selected", Json::array());
        if (!filter.is_object() || text.size() > 16384 || !ids.is_array() || ids.size() > 100000)
            return;
        std::set<Id> selected;
        for (const auto& id : ids) {
            if (!id.is_number_integer() || id.get<Id>() <= 0)
                return;
            selected.insert(id.get<Id>());
        }
        const auto offset = saved.value("offset", std::int64_t{});
        const auto loaded = saved.value("loaded", offset + static_cast<std::int64_t>(page_size_));
        const auto cursor = saved.value("cursor", Id{});
        const auto saved_search = saved.value("saved_search_id", Id{});
        auto collection_name = saved.value("collection_name", std::string{});
        if (offset < 0 || offset > 100000000 || loaded < 0 || loaded > 100001000 || cursor < 0 ||
            saved_search < 0 || collection_name.size() > 16384)
            return;
        const auto identity = saved.value("catalog_identity", std::string{});
        if (identity.size() > 128)
            return;
        clear_catalog_state();
        search_ = std::move(text);
        filter_ = std::move(filter);
        selected_ = std::move(selected);
        restored_selection_pending_ = true;
        offset_ = static_cast<std::size_t>(loaded);
        cursor_ = cursor;
        active_saved_search_ = saved_search;
        collection_name_ = std::move(collection_name);
        restored_catalog_identity_ = saved.value("version", 0) == 3 ? identity : std::string{};
        restored_catalog_state_ = true;
        if (restored_catalog_identity_.empty() ||
            (catalog_identity_ready_ && restored_catalog_identity_ != catalog_identity_)) {
            clear_catalog_state();
        } else if (catalog_identity_ready_) {
            restored_catalog_state_ = false;
        }
        state_dirty_ = true;
    } catch (const Json::exception&) {
    }
}

void PhotoLibrary::refresh_navigation() {
    if (!catalog_identity_ready_) {
        initialize_catalog();
        return;
    }
    if (!client_->available())
        return;
    if (navigation_loading_) {
        navigation_again_ = true;
        return;
    }
    navigation_loading_ = true;
    const auto generation = ++navigation_generation_;

    struct PendingNavigation {
        Json snapshot = Json::object();
        int remaining{3};
        bool failed{false};
    };

    const auto pending = std::make_shared<PendingNavigation>();
    for (const std::string op : {"albums", "filters", "sources"})
        request({{"op", op}}, [this, generation, pending, op](Json response) {
            if (generation != navigation_generation_)
                return;
            if (response.value("ok", false) && response.contains(op) && response[op].is_array())
                pending->snapshot[op] = std::move(response[op]);
            else
                pending->failed = true;
            if (--pending->remaining != 0)
                return;
            navigation_loading_ = false;
            // A successful mutation during the read invalidates this snapshot. Publish only
            // the subsequent read, never a mixture from before and after that mutation.
            if (std::exchange(navigation_again_, false)) {
                refresh_navigation();
                return;
            }
            if (pending->failed)
                return;  // Keep the last complete navigation snapshot on an I/O error.
            const bool changed = !navigation_ready_ || navigation_ != pending->snapshot;
            navigation_ = std::move(pending->snapshot);
            navigation_ready_ = true;
            if (changed && navigation_changed_)
                navigation_changed_(navigation_);
        });
}

std::string PhotoLibrary::navigation_uri() const {
    constexpr std::string_view root = "rummage:photos";
    switch (destination_) {
        case Destination::sources:
            return std::string(root) + "/sources";
        case Destination::albums:
            return std::string(root) + "/albums";
        case Destination::filters:
            return std::string(root) + "/searches";
        case Destination::review:
        case Destination::duplicate_group:
            return std::string(root) + "/review";
        case Destination::activity:
            return std::string(root) + "/activity";
        case Destination::library:
            break;
    }
    if (active_saved_search_ > 0)
        return std::format("{}/searches/{}", root, active_saved_search_);
    const auto filter_id = [this](const char* key) {
        const auto found = filter_.find(key);
        return found != filter_.end() && found->is_number_integer() ? found->get<Id>() : Id{};
    };
    if (const auto id = filter_id("album_id"); id > 0)
        return std::format("{}/albums/{}", root, id);
    if (const auto id = filter_id("source_id"); id > 0)
        return std::format("{}/sources/{}", root, id);
    return std::string(root);
}

PhotoLibrary::Id PhotoLibrary::active_album() const {
    return destination_ == Destination::library && active_saved_search_ == 0
               ? filter_.value("album_id", Id{})
               : 0;
}

void PhotoLibrary::add_album_paths(Id album, const std::vector<std::filesystem::path>& paths) {
    Json values = Json::array();
    for (const auto& path : paths)
        values.push_back(path.string());
    mutate({{"op", "album_add_paths"}, {"id", album}, {"paths", std::move(values)}},
           tr("Photos added to album; original files kept in place"));
}

void PhotoLibrary::open_album(Id id, std::string name) {
    discard_pending_restore();
    if (id <= 0)
        return;
    sync_selection();
    auto next_filter = sort_filter(filter_);
    next_filter["album_id"] = id;
    if (filter_ != next_filter || !search_.empty() || active_saved_search_ > 0 ||
        destination_ == Destination::duplicate_group)
        clear_photo_selection();
    active_saved_search_ = 0;
    filter_ = std::move(next_filter);
    search_.clear();
    collection_name_ = std::move(name);
    offset_ = 0;
    show(Destination::library);
}

void PhotoLibrary::open_source(Id id, std::string path) {
    discard_pending_restore();
    if (id <= 0)
        return;
    sync_selection();
    auto next_filter = sort_filter(filter_);
    next_filter["source_id"] = id;
    if (filter_ != next_filter || !search_.empty() || active_saved_search_ > 0 ||
        destination_ == Destination::duplicate_group)
        clear_photo_selection();
    active_saved_search_ = 0;
    filter_ = std::move(next_filter);
    search_.clear();
    collection_name_ = std::move(path);
    offset_ = 0;
    show(Destination::library);
}

void PhotoLibrary::open_saved_search(Json record) {
    discard_pending_restore();
    if (!record.is_object() || !record.contains("filter") || !record["filter"].is_object())
        return;
    sync_selection();
    if (filter_ != record["filter"] || active_saved_search_ != record.value("id", Id{}) ||
        search_ != record["filter"].value("text", "") ||
        destination_ == Destination::duplicate_group)
        clear_photo_selection();
    active_saved_search_ = record.value("id", Id{});
    filter_ = record["filter"];
    search_ = filter_.value("text", "");
    collection_name_ = record.value("name", "");
    offset_ = 0;
    show(Destination::library);
}

void PhotoLibrary::clear_catalog_state() {
    query_job_.cancel();
    ++generation_;
    querying_ = false;
    selected_.clear();
    selected_snapshots_.clear();
    cursor_ = active_saved_search_ = 0;
    offset_ = total_ = 0;
    filter_ = sort_filter(filter_);
    collection_name_.clear();
    destination_ = Destination::library;
    rows_ = records_ = Json::array();
    row_by_id_.clear();
    record_by_key_.clear();
    pending_metadata_.clear();
    ++data_revision_;
    publishing_ = true;
    folder_->replace_collection({}, title(), 0);
    publishing_ = false;
    selection_revision_ = folder_->selection_revision();
    restored_catalog_state_ = false;
    restored_selection_pending_ = false;
    restored_catalog_identity_.clear();
    state_dirty_ = true;
}

void PhotoLibrary::discard_pending_restore() {
    if (restored_catalog_state_ && !catalog_identity_ready_)
        clear_catalog_state();
}

void PhotoLibrary::initialize_catalog() {
    if (catalog_identity_ready_ || catalog_identity_loading_ || !client_->available())
        return;
    catalog_identity_loading_ = true;
    request({{"op", "catalog_identity"}}, [this](Json response) {
        catalog_identity_loading_ = false;
        if (!response.value("ok", false))
            return;
        const auto identity = response.value("identity", std::string{});
        if (identity.empty()) {
            report(tr("Cannot identify the photo catalog"));
            return;
        }
        if (restored_catalog_state_ && restored_catalog_identity_ != identity)
            clear_catalog_state();
        else if (restored_catalog_state_) {
            restored_catalog_state_ = false;
        }
        catalog_identity_ = identity;
        catalog_identity_ready_ = true;
        // Restoring the tab's sort may change the empty Folder revision before validation.
        // It is not a user selection change and must not clear the bound restored cursor.
        selection_revision_ = folder_->selection_revision();
        state_dirty_ = true;
        refresh_navigation();
        if (std::exchange(resume_requested_, false))
            resume();
        if (std::exchange(pending_album_creation_, false) && open_)
            create_album(selected_ids());
    });
}

void PhotoLibrary::create_album_when_ready() {
    if (catalog_identity_ready_)
        create_album(selected_ids());
    else {
        pending_album_creation_ = true;
        initialize_catalog();
    }
}

void PhotoLibrary::open() {
    open_ = true;
    if (!client_->available()) {
        report(client_->error());
        return;
    }
    if (catalog_identity_ready_)
        refresh_navigation();
    resume();
}

void PhotoLibrary::resume() {
    if (!catalog_identity_ready_) {
        resume_requested_ = true;
        initialize_catalog();
        return;
    }
    if (!source_watcher_)
        source_watcher_ = std::make_unique<photos::SourceWatcher>(loop_, pool_, watcher_, client_,
                                                                  [this](const std::string& error) {
                                                                      if (!error.empty())
                                                                          report(error);
                                                                      else {
                                                                          if (open_)
                                                                              refresh();
                                                                          schedule_work();
                                                                      }
                                                                  });
    source_watcher_->refresh_subscriptions();
    request({{"op", "recover_operations"}}, [this](Json) {
        if (open_)
            refresh();
        schedule_work();
    });
}

void PhotoLibrary::close() {
    open_ = false;
    close_form();
    confirm_.close();
    changed();
}

void PhotoLibrary::request(Json input, std::function<void(Json)> done, core::Priority priority) {
    if (!client_->available()) {
        report(client_->error());
        return;
    }
    const auto task = ++task_ids_;
    pending_.emplace(
        task, pool_.submit(
                  priority,
                  [client = client_, input = std::move(input)](const core::CancelToken& token) {
                      return client->request(input, &token);
                  },
                  [this, alive = alive_, task, done = std::move(done)](Json response) {
                      if (!*alive)
                          return;
                      pending_.erase(task);
                      if (!response.value("ok", false)) {
                          message_ = response.value("error", tr("Photo Library request failed"));
                          form_message_ = message_;
                      }
                      done(std::move(response));
                      changed();
                  }));
}

fs::Entry PhotoLibrary::photo_entry(const Json& row) const {
    fs::Entry entry;
    entry.name = row.value("filename", "");
    entry.kind = fs::EntryKind::file;
    entry.size = row.value("size", std::uint64_t{});
    entry.modified_ns = row.value("mtime_ns", std::int64_t{});
    entry.modified_known = row.value("mtime_ns_valid", true);
    entry.mime = mime_->by_name(entry.name).mime;
    entry.name_guess = entry.mime;
    entry.identity = "photo:" + std::to_string(row.value("id", Id{}));
    entry.location = row.value("path", "");
    entry.available = row.value("availability", "") == "online";
    entry.preview_current_location = row.value("availability", "") == "changed";
    return entry;
}

std::string PhotoLibrary::record_key(const Json& row, std::size_t index) const {
    if (destination_ == Destination::review)
        return "group:" + row.value("key", std::to_string(index));
    const auto prefix = destination_ == Destination::sources   ? "source:"
                        : destination_ == Destination::albums  ? "album:"
                        : destination_ == Destination::filters ? "filter:"
                        : row.value("file_operation", false)   ? "operation:"
                                                               : "job:";
    return prefix + std::to_string(row.value("id", Id{}));
}

void PhotoLibrary::index_rows() {
    row_by_id_.clear();
    row_by_id_.reserve(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i)
        row_by_id_.emplace(rows_[i].value("id", Id{}), i);
    ++data_revision_;
}

void PhotoLibrary::publish() {
    index_rows();
    sync_selection();
    publishing_ = true;
    std::vector<fs::Entry> entries;
    record_by_key_.clear();
    if (photo_rows()) {
        for (const auto& row : rows_)
            if (selected_.contains(row.value("id", Id{})))
                selected_snapshots_[row.value("id", Id{})] = row;
        entries.reserve(rows_.size());
        for (const auto& row : rows_)
            entries.push_back(photo_entry(row));
    } else {
        entries.reserve(records_.size());
        for (std::size_t i = 0; i < records_.size(); ++i) {
            const auto& row = records_[i];
            fs::Entry entry;
            entry.kind = fs::EntryKind::other;
            entry.available = false;
            if (destination_ == Destination::albums && row.contains("cover") &&
                row["cover"].is_object() && !row["cover"].empty())
                entry = photo_entry(row["cover"]);
            entry.identity = record_key(row, i);
            if (destination_ == Destination::sources)
                entry.name = row.value("path", "");
            else if (destination_ == Destination::albums || destination_ == Destination::filters)
                entry.name = row.value("name", "");
            else if (destination_ == Destination::review)
                entry.name = i18n::trn("{} duplicate candidate", "{} duplicate candidates",
                                       row.value("photos", Json::array()).size());
            else
                entry.name = std::format("{} · {}", photo_label(row.value("kind", "Job")),
                                         photo_label(row.value("state", "")));
            record_by_key_.emplace(entry.identity, row);
            entries.push_back(std::move(entry));
        }
    }
    folder_->replace_collection(std::move(entries), title(),
                                photo_rows() ? total_ : records_.size(), querying_);
    if (photo_rows()) {
        std::vector<std::size_t> selected_rows;
        std::optional<std::size_t> cursor;
        for (std::size_t i = 0; i < folder_->entries().size(); ++i) {
            const auto id = photo_id(folder_->entries()[i].key());
            if (selected_.contains(id))
                selected_rows.push_back(i);
            if (id == cursor_)
                cursor = i;
        }
        std::vector<std::string> desired;
        for (const auto row : selected_rows)
            desired.push_back(folder_->entries()[row].key());
        if (folder_->selected_names() != desired) {
            if (desired.empty())
                folder_->set_cursor(std::nullopt);
            else
                folder_->select_rows(selected_rows, false);
        }
        if (cursor && folder_->cursor() != cursor)
            folder_->set_cursor(cursor, SelectMode::keep);
    }
    selection_revision_ = folder_->selection_revision();
    publishing_ = false;
    changed();
}

void PhotoLibrary::publish_append(std::size_t first) {
    publishing_ = true;
    std::vector<fs::Entry> entries;
    std::vector<std::size_t> newly_selected;
    std::optional<std::size_t> cursor;
    entries.reserve(rows_.size() - first);
    for (std::size_t i = first; i < rows_.size(); ++i) {
        const auto id = rows_[i].value("id", Id{});
        row_by_id_[id] = i;
        entries.push_back(photo_entry(rows_[i]));
        if (selected_.contains(id)) {
            selected_snapshots_[id] = rows_[i];
            newly_selected.push_back(i);
        }
        if (id == cursor_)
            cursor = i;
    }
    ++data_revision_;
    folder_->append_collection(std::move(entries), total_, querying_);
    if (!newly_selected.empty())
        folder_->select_band(folder_->selected_names(), newly_selected);
    if (cursor)
        folder_->set_cursor(cursor, SelectMode::keep);
    selection_revision_ = folder_->selection_revision();
    publishing_ = false;
    changed();
}

void PhotoLibrary::update_metadata(const Json& ids) {
    if (!photo_rows())
        return;
    for (const auto& id : ids)
        if (id.is_number_integer() && row_by_id_.contains(id.get<Id>()))
            pending_metadata_.insert(id.get<Id>());
    if (!ids.empty() && destination_ == Destination::library &&
        (filter_.value("sort", "name") == "date" || filter_.value("unknown_date", false) ||
         !filter_.value("date_from", "").empty() || !filter_.value("date_to", "").empty())) {
        metadata_refresh_pending_ = true;
        if (metadata_refresh_after_ == std::chrono::steady_clock::time_point{})
            metadata_refresh_after_ = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    }
    drain_metadata();
}

void PhotoLibrary::drain_metadata() {
    if (querying_ || metadata_querying_ || metadata_retry_timer_ || pending_metadata_.empty() ||
        !photo_rows())
        return;
    Json ids = Json::array();
    while (!pending_metadata_.empty() && ids.size() < 1000) {
        ids.push_back(*pending_metadata_.begin());
        pending_metadata_.erase(pending_metadata_.begin());
    }
    metadata_querying_ = true;
    const auto generation = generation_;
    request({{"op", "query_ids"}, {"ids", ids}, {"include_inactive", true}},
            [this, generation, ids](Json response) {
                metadata_querying_ = false;
                if (generation != generation_)
                    for (const auto& id : ids)
                        if (row_by_id_.contains(id.get<Id>()))
                            pending_metadata_.insert(id.get<Id>());
                if (generation == generation_ && !response.value("ok", false)) {
                    for (const auto& id : ids)
                        if (row_by_id_.contains(id.get<Id>()))
                            pending_metadata_.insert(id.get<Id>());
                    report(response.value("error", tr("Cannot query Photo Library")));
                    if (++metadata_retry_count_ <= 3)
                        metadata_retry_timer_ = loop_.add_timer(
                            std::chrono::seconds{1}, std::chrono::milliseconds{0}, [this] {
                                metadata_retry_timer_ = 0;
                                drain_metadata();
                            });
                    return;
                }
                if (generation == generation_ && response.value("ok", false)) {
                    metadata_retry_count_ = 0;
                    if (!response.value("missing_ids", Json::array()).empty()) {
                        refresh();
                        return;
                    }
                    std::vector<std::pair<std::size_t, fs::Entry>> patches;
                    for (auto& row : response.at("photos")) {
                        const auto id = row.value("id", Id{});
                        const auto found = row_by_id_.find(id);
                        if (found == row_by_id_.end())
                            continue;
                        const auto index = found->second;
                        rows_[index] = std::move(row);
                        if (selected_.contains(id))
                            selected_snapshots_[id] = rows_[index];
                        patches.emplace_back(index, photo_entry(rows_[index]));
                    }
                    if (!patches.empty()) {
                        publishing_ = true;
                        const bool patched = folder_->update_collection_rows(std::move(patches));
                        publishing_ = false;
                        if (!patched) {
                            refresh();
                            return;
                        }
                        ++data_revision_;
                        changed();
                    }
                }
                drain_metadata();
            });
}

void PhotoLibrary::query(bool reset) {
    if (!catalog_identity_ready_) {
        initialize_catalog();
        return;
    }
    sync_selection();
    query_job_.cancel();
    const auto generation = ++generation_;
    const auto wanted = reset ? page_size_ : std::max({page_size_, rows_.size(), offset_});
    querying_ = true;
    refresh_again_ = false;
    append_failed_ = false;
    if (reset) {
        rows_ = Json::array();
        total_ = offset_ = 0;
        publish();
    }
    Json input = filter_;
    input["op"] = "query";
    input["text"] = search_;
    query_job_ = pool_.catalog_lane().submit(
        core::Priority::visible,
        [client = client_, input, wanted](const core::CancelToken& token) mutable {
            Json collected = Json::array();
            Json response;
            do {
                if (token.cancelled())
                    return Json{{"ok", false}, {"error", tr("Query cancelled")}};
                input["offset"] = collected.size();
                input["limit"] = std::min(std::size_t{1000}, wanted - collected.size());
                response = client->request(input, &token);
                if (!response.value("ok", false))
                    return response;
                auto& batch = response.at("photos");
                if (batch.empty())
                    break;
                for (auto& row : batch)
                    collected.push_back(std::move(row));
            } while (collected.size() < wanted &&
                     collected.size() < response.value("total", std::size_t{}));
            response["fetched"] = collected.size();
            response["photos"] = std::move(collected);
            return response;
        },
        [this, alive = alive_, generation](Json response) {
            if (!*alive || generation != generation_)
                return;
            querying_ = false;
            if (response.value("ok", false)) {
                sync_selection();
                rows_ = Json::array();
                std::set<Id> seen;
                for (auto& row : response.at("photos"))
                    if (seen.insert(row.value("id", Id{})).second)
                        rows_.push_back(std::move(row));
                offset_ = response.value("fetched", std::size_t{});
                total_ = response.value("total", std::size_t{});
                if (std::exchange(restored_selection_pending_, false)) {
                    // Old sessions could carry picks from unrelated collections. The restored
                    // loaded prefix is complete now; only its visible identities remain targets.
                    std::erase_if(selected_, [&](Id id) { return !seen.contains(id); });
                    std::erase_if(selected_snapshots_, [&](const auto& item) {
                        return !selected_.contains(item.first);
                    });
                    if (!seen.contains(cursor_))
                        cursor_ = 0;
                    selected_properties_.reset();
                    state_dirty_ = true;
                }
                publish();
                drain_metadata();
                if (std::exchange(refresh_again_, false))
                    refresh();
            } else {
                append_failed_ = true;
                report(response.value("error", tr("Cannot query Photo Library")));
            }
        });
    changed();
}

void PhotoLibrary::load_more() {
    if (!open_ || destination_ != Destination::library || querying_ || append_failed_ ||
        offset_ >= total_)
        return;
    sync_selection();
    querying_ = true;
    const auto generation = ++generation_;
    Json input = filter_;
    input["op"] = "query";
    input["text"] = search_;
    input["offset"] = offset_;
    input["limit"] = page_size_;
    query_job_ = pool_.catalog_lane().submit(
        core::Priority::visible,
        [client = client_, input](const core::CancelToken& token) {
            return client->request(input, &token);
        },
        [this, alive = alive_, generation](Json response) {
            if (!*alive || generation != generation_)
                return;
            querying_ = false;
            if (!response.value("ok", false)) {
                append_failed_ = true;
                report(response.value("error", tr("Cannot load more photos. Refresh to retry.")));
                return;
            }
            sync_selection();
            auto batch =
                response.contains("photos") ? std::move(response.at("photos")) : Json::array();
            offset_ += batch.size();
            total_ = response.value("total", std::size_t{});
            if (batch.empty())
                offset_ = total_;
            const auto first = rows_.size();
            for (auto& row : batch) {
                const auto id = row.value("id", Id{});
                if (!row_by_id_.contains(id)) {
                    row_by_id_[id] = rows_.size();
                    rows_.push_back(std::move(row));
                }
            }
            publish_append(first);
            drain_metadata();
            if (std::exchange(refresh_again_, false) ||
                (offset_ >= total_ && rows_.size() < total_))
                refresh();
        });
    changed();
}

void PhotoLibrary::refresh() {
    if (!catalog_identity_ready_) {
        initialize_catalog();
        return;
    }
    if (destination_ == Destination::library) {
        if (querying_) {
            refresh_again_ = true;
            return;
        }
        query();
        refresh_progress();
        return;
    }
    if (destination_ == Destination::duplicate_group) {
        refresh_progress();
        return;
    }
    const auto destination = destination_;
    const auto generation = ++generation_;
    const std::string op = destination == Destination::sources   ? "sources"
                           : destination == Destination::albums  ? "albums"
                           : destination == Destination::filters ? "filters"
                           : destination == Destination::review  ? "duplicates"
                                                                 : "jobs";
    querying_ = true;
    Json input{{"op", op}};
    if (destination == Destination::review) {
        input["offset"] = review_offset_;
        input["limit"] = page_size_;
    }
    request(std::move(input), [this, destination, generation, op](Json response) {
        if (generation != generation_ || destination != destination_)
            return;
        querying_ = false;
        if (!response.value("ok", false))
            return;
        records_ = response.value(op == "duplicates" ? "groups" : op, Json::array());
        if (destination == Destination::review) {
            review_total_ = response.value("total", records_.size());
            if (records_.empty() && review_offset_ > 0 && review_total_ > 0) {
                review_offset_ = ((review_total_ - 1) / page_size_) * page_size_;
                refresh();
                return;
            }
        }
        if (destination == Destination::activity) {
            update_background(response);
            update_progress(records_);
            request({{"op", "operations"}}, [this, generation](Json result) {
                if (generation != generation_ || destination_ != Destination::activity ||
                    !result.value("ok", false))
                    return;
                Json joined = Json::array();
                for (auto row : result.value("operations", Json::array())) {
                    row["file_operation"] = true;
                    joined.push_back(std::move(row));
                }
                for (auto row : records_)
                    joined.push_back(std::move(row));
                records_ = std::move(joined);
                publish();
            });
        }
        publish();
    });
    refresh_progress();
}

bool PhotoLibrary::at_all_photos() const {
    if (destination_ != Destination::library || !search_.empty() || active_saved_search_ > 0)
        return false;
    for (const auto& [key, value] : filter_.items()) {
        (void)value;
        if (key != "sort" && key != "descending")
            return false;
    }
    return true;
}

void PhotoLibrary::show_all_photos() {
    discard_pending_restore();
    sync_selection();
    const auto next_filter = sort_filter(filter_);
    const bool changed_scope = filter_ != next_filter || !search_.empty() ||
                               active_saved_search_ > 0 ||
                               destination_ == Destination::duplicate_group;
    if (!changed_scope && destination_ == Destination::library) {
        refresh();
        return;
    }
    if (changed_scope)
        clear_photo_selection();
    active_saved_search_ = 0;
    filter_ = next_filter;
    search_.clear();
    collection_name_.clear();
    offset_ = 0;
    message_.clear();
    show(Destination::library);
}

void PhotoLibrary::show(Destination destination) {
    discard_pending_restore();
    sync_selection();
    query_job_.cancel();
    ++generation_;
    if (destination == Destination::review && destination_ != Destination::duplicate_group)
        review_offset_ = 0;
    destination_ = destination;
    pending_metadata_.clear();
    metadata_refresh_pending_ = false;
    refresh_again_ = false;
    records_ = Json::array();
    close_form();
    confirm_.close();
    if (destination != Destination::library && destination != Destination::duplicate_group) {
        publishing_ = true;
        folder_->replace_collection({}, title(), 0, true);
        publishing_ = false;
        selection_revision_ = folder_->selection_revision();
    }
    if (destination == Destination::library) {
        query(true);
        refresh_progress();
    } else
        refresh();
    changed();
}

void PhotoLibrary::set_query(std::string query_text) {
    if (query_text == search_)
        return;
    cancel_album_chooser();
    sync_selection();
    search_ = std::move(query_text);
    if (active_saved_search_ > 0)
        collection_name_.clear();
    active_saved_search_ = 0;
    offset_ = 0;
    clear_photo_selection();
    if (destination_ != Destination::library)
        destination_ = Destination::library;
    query(true);
}

void PhotoLibrary::set_sort(const fs::SortOrder& order) {
    order_ = order;
    folder_->set_order(order);
    if (destination_ != Destination::library)
        return;
    filter_["sort"] = order.key == fs::SortKey::size       ? "size"
                      : order.key == fs::SortKey::modified ? "modified"
                      : order.key == fs::SortKey::location ? "location"
                                                           : "name";
    filter_["descending"] = order.descending;
    query();
}

void PhotoLibrary::schedule_work(bool busy) {
    if (work_timer_ || working_ || !client_->available())
        return;
    work_timer_ = loop_.add_timer(std::chrono::milliseconds{busy ? 1000 : work_delay_ms_},
                                  std::chrono::milliseconds{0}, [this] {
                                      work_timer_ = 0;
                                      work();
                                  });
}

void PhotoLibrary::work() {
    if (working_)
        return;
    working_ = true;
    const auto background_generation = background_generation_;
    request(
        {{"op", "work"}, {"budget", 32}},
        [this, background_generation](Json response) {
            working_ = false;
            if (response.value("ok", false)) {
                if (background_generation == background_generation_)
                    update_background(response);
                if (response.value("paused", false)) {
                    if (open_)
                        refresh_progress();
                    schedule_work(true);
                } else {
                    if (response.value("topology_changed", false) && source_watcher_)
                        source_watcher_->refresh_subscriptions();
                    if (open_) {
                        if (response.value("discovery_changed", false))
                            refresh();
                        else
                            update_metadata(response.value("changed_ids", Json::array()));
                        if (response.value("jobs_changed", false) ||
                            response.value("worked", false))
                            refresh_progress();
                        if (metadata_refresh_pending_ &&
                            (!response.value("worked", false) ||
                             std::chrono::steady_clock::now() >= metadata_refresh_after_)) {
                            metadata_refresh_pending_ = false;
                            metadata_refresh_after_ =
                                std::chrono::steady_clock::now() + std::chrono::seconds{10};
                            refresh();
                        }
                        if (destination_ == Destination::activity &&
                            response.value("jobs_changed", false))
                            refresh();
                    }
                    if (response.value("worked", false))
                        schedule_work();
                    else if (response.value("busy", false))
                        schedule_work(true);
                }
            }
        },
        core::Priority::prefetch);
}

void PhotoLibrary::mutate(Json input, std::string success,
                          std::function<void(std::string)> failure) {
    const auto op = input.value("op", "");
    const auto album_id = input.value("id", Id{});
    const auto album_name = input.value("name", std::string{});
    const bool navigation_changed =
        op == "album_add_paths" || op == "rename_album" || op == "delete_album" ||
        op == "set_album_cover" || op == "create_album" || op == "save_filter" || op == "restore" ||
        op == "add_source" || op == "remove_source" || op == "forget_source" ||
        op == "relink_source" || op == "refresh" ||
        (op == "edit" && (input.contains("add_albums") || input.contains("remove_albums")));
    const bool watches_changed = op == "add_source" || op == "remove_source" ||
                                 op == "forget_source" || op == "relink_source" || op == "refresh";
    request(std::move(input), [this, op, album_id, album_name, navigation_changed, watches_changed,
                               form_generation = form_generation_, success = std::move(success),
                               failure = std::move(failure)](Json response) {
        if (!response.value("ok", false)) {
            if (failure)
                failure(response.value("error", tr("Photo Library request failed")));
            return;
        }
        message_ = success;
        if (filter_.value("album_id", Id{}) == album_id && album_id > 0) {
            if (op == "rename_album" && active_saved_search_ == 0)
                collection_name_ = album_name;
            else if (op == "delete_album") {
                filter_ = sort_filter(filter_);
                active_saved_search_ = 0;
                collection_name_.clear();
                offset_ = 0;
                if (destination_ == Destination::library)
                    show(Destination::albums);
            }
        }
        if (navigation_changed)
            refresh_navigation();
        if (response.contains("missing") || response.contains("ambiguous"))
            message_ = i18n::trf("Restored: {} · Missing: {} · Ambiguous: {}",
                                 response.value("restored", 0),
                                 response.value("missing", Json::array()).size(),
                                 response.value("ambiguous", Json::array()).size());
        if (response.contains("sources_requiring_refresh") &&
            !response["sources_requiring_refresh"].empty())
            message_ +=
                " · " + i18n::trf("{} restored sources need explicit Refresh before discovery",
                                  response["sources_requiring_refresh"].size());
        if (form_generation == form_generation_)
            close_form();
        if (watches_changed && source_watcher_)
            source_watcher_->refresh_subscriptions();
        refresh();
        schedule_work();
    });
}

void PhotoLibrary::refresh_progress() {
    const auto generation = ++progress_generation_;
    const auto background_generation = background_generation_;
    request({{"op", "progress"}}, [this, generation, background_generation](Json response) {
        if (generation == progress_generation_ && response.value("ok", false)) {
            if (background_generation == background_generation_)
                update_background(response);
            update_progress(response.value("jobs", Json::array()),
                            response.value("job_counts", Json::object()));
        }
    });
}

void PhotoLibrary::update_background(const Json& response) {
    if (!response.contains("paused"))
        return;
    background_ready_ = true;
    background_paused_ = response.value("paused", false);
    background_pace_ = response.value("pace", "normal");
    work_delay_ms_ = std::clamp(response.value("delay_ms", 250), 50, 5000);
}

void PhotoLibrary::control_background(Json changes) {
    const auto generation = ++background_generation_;
    const bool pause_changed = changes.contains("paused");
    changes["op"] = "background_control";
    request(std::move(changes), [this, generation, pause_changed](Json response) {
        if (generation != background_generation_ || !response.value("ok", false))
            return;
        update_background(response);
        message_ = !pause_changed ? (background_pace_ == "gentle"
                                         ? tr("Gentle pace uses smaller, less frequent batches.")
                                         : tr("Normal background pace restored."))
                   : background_paused_
                       ? tr("Background work paused. In-flight items finish; no new items start.")
                       : tr("Background work resumed. Individually paused jobs stay paused.");
        loop_.remove(work_timer_);
        work_timer_ = 0;
        refresh();
        schedule_work();
    });
}

void PhotoLibrary::update_progress(const Json& jobs, const Json& counts) {
    if (jobs.empty()) {
        progress_ = tr("No background jobs. Add a folder when you are ready.");
        return;
    }
    const Json* shown = &jobs.front();
    std::size_t paused = 0, waiting = 0, failed = 0;
    for (const auto& job : jobs) {
        const auto state = job.value("state", "");
        if (state == "running" || state == "queued")
            shown = &job;
        paused += state == "paused" ? 1U : 0U;
        waiting += state == "waiting" ? 1U : 0U;
        failed += state == "failed" ? 1U : 0U;
    }
    paused = counts.value("paused", paused);
    waiting = counts.value("waiting", waiting);
    failed = counts.value("failed", failed);
    const auto kind = shown->value("kind", "");
    const auto phase = shown->value("phase", "");
    if (kind == "scan" && !phase.empty()) {
        const auto state = photo_label(shown->value("state", ""));
        if (phase == "discovering")
            progress_ = i18n::trf("Finding photos: {} · {} entries checked · {} folders pending",
                                  state, shown->value("completed", Id{}),
                                  shown->value("pending_directories", Id{}));
        else if (phase == "metadata")
            progress_ = i18n::trf("Reading metadata: {} · {} / {} eligible photos checked", state,
                                  shown->value("metadata_completed", Id{}),
                                  shown->value("metadata_total", Id{}));
        else
            progress_ =
                tr("Index photos") + ": " + (phase == "finishing" ? tr("Finishing") : state);
        const auto unavailable = shown->value("metadata_unavailable", Id{});
        if (unavailable > 0)
            progress_ +=
                " · " + i18n::trf("{} changed or unavailable — refresh source", unavailable);
    } else {
        const auto title = kind == "duplicates" ? tr("Find exact duplicates") : tr("Index photos");
        progress_ =
            i18n::trf("{}: {} · {} items completed", title, photo_label(shown->value("state", "")),
                      shown->value("completed", Id{}));
        if (shown->contains("total") && !(*shown)["total"].is_null())
            progress_ += " / " + (*shown)["total"].dump();
        else
            progress_ += " · " + tr("Total not yet known");
    }
    const auto in_flight = shown->value("in_flight", 0);
    if (in_flight > 0)
        progress_ += " · " + i18n::trf("{} in flight", in_flight);
    if (paused || waiting || failed)
        progress_ += " · " + i18n::trf("{} paused · {} waiting · {} failed — check system journal",
                                       paused, waiting, failed);
}

PhotoProperties PhotoLibrary::properties(bool focused_only) const {
    const auto* entry = folder_->cursor_entry();
    // A focused inspector never expands to the bulk selection when no row is focused.
    if (focused_only && !entry)
        return {};
    if (!focused_only)
        sync_selection();
    auto& cache = focused_only ? focused_properties_ : selected_properties_;
    const auto key = entry ? entry->key() : std::string{};
    const auto revision = focused_only ? 0 : selection_revision_;
    if (!cache || cache->data_revision != data_revision_ || cache->selection_revision != revision ||
        cache->cursor != key || cache->language != i18n::language()) {
        cache = PropertiesCache{data_revision_, revision, key, std::string{i18n::language()},
                                build_properties(focused_only)};
    }
    return cache->value;
}

PhotoProperties PhotoLibrary::build_properties(bool focused_only) const {
    PhotoProperties result;
    if (!photo_rows()) {
        const auto row = focused_record();
        const auto* entry = folder_->cursor_entry();
        result.title = entry ? entry->name : title();
        result.available = false;
        if (entry)
            result.entry = *entry;
        for (const auto& [key, label] : std::array{
                 std::pair{"path", tr("Folder")}, std::pair{"availability", tr("Availability")},
                 std::pair{"recursive", tr("Include subfolders")},
                 std::pair{"exclusions", tr("Excluded folders")}, std::pair{"name", tr("Name")},
                 std::pair{"count", tr("Visible photos")},
                 std::pair{"retained_count", tr("Retained associations")},
                 std::pair{"kind", tr("Job")}, std::pair{"state", tr("State")},
                 std::pair{"completed", tr("Completed")}, std::pair{"total", tr("Total")},
                 std::pair{"phase", tr("Phase")},
                 std::pair{"pending_directories", tr("Folders pending")},
                 std::pair{"metadata_completed", tr("Metadata checked")},
                 std::pair{"metadata_pending", tr("Metadata pending")},
                 std::pair{"metadata_unavailable", tr("Changed or unavailable")},
                 std::pair{"error", tr("Error")}, std::pair{"reason", tr("Review reason")}}) {
            const auto value = std::string_view{key} == "recursive" && row.contains(key)
                                   ? std::string{row.value(key, false) ? tr("Yes") : tr("No")}
                                   : text_value(row, key);
            if (!value.empty())
                result.rows.emplace_back(label, std::string_view{key} == "availability" ||
                                                        std::string_view{key} == "state" ||
                                                        std::string_view{key} == "phase" ||
                                                        std::string_view{key} == "kind"
                                                    ? photo_label(value)
                                                    : value);
        }
        return result;
    }
    const auto* entry = folder_->cursor_entry();
    const auto at = entry ? row_by_id_.find(photo_id(entry->key())) : row_by_id_.end();
    const Json* focused = at == row_by_id_.end() ? nullptr : &rows_[at->second];
    if (focused_only) {
        if (!focused)
            return result;
        result.ids.push_back(focused->value("id", Id{}));
    } else {
        result.ids.assign(selected_.begin(), selected_.end());
    }
    std::vector<const Json*> selected;
    selected.reserve(result.ids.size());
    for (const auto id : result.ids) {
        if (const auto found = row_by_id_.find(id); found != row_by_id_.end())
            selected.push_back(&rows_[found->second]);
        else if (const auto saved = selected_snapshots_.find(id);
                 saved != selected_snapshots_.end())
            selected.push_back(&saved->second);
    }
    result.title = result.ids.size() == 1 && !selected.empty()
                       ? selected.front()->value("filename", "")
                       : i18n::trn("{} selected photo", "{} selected photos", result.ids.size());
    if (result.ids.empty())
        result.title = title();
    const Json* shown_row = focused ? focused : selected.empty() ? nullptr : selected.front();
    if (shown_row) {
        const auto& shown = *shown_row;
        result.entry = photo_entry(shown);
        result.path = result.entry.location;
        result.available = result.entry.available;
        if (destination_ == Destination::duplicate_group)
            result.rows.emplace_back(tr("Decision"), keepers_.contains(shown.value("id", Id{}))
                                                         ? tr("Keep")
                                                         : tr("Marked for trash"));
        result.rows.emplace_back(tr("Availability"), photo_label(shown.value("availability", "")));
        if (!shown.value("mtime_ns_valid", true))
            result.rows.emplace_back(tr("Modified"),
                                     tr("Modified time is outside the supported range"));
        if (result.entry.preview_current_location)
            result.rows.emplace_back(
                tr("Preview"), tr("Current file at this location; catalog identity has changed"));
        result.rows.emplace_back(tr("Location"), shown.value("path", ""));
        const bool changed = result.entry.preview_current_location;
        const auto width = shown.value("width", 0), height = shown.value("height", 0);
        if (width > 0 && height > 0)
            result.rows.emplace_back(changed ? tr("Catalog dimensions") : tr("Dimensions"),
                                     std::format("{} × {}", width, height));
        const auto camera = text_value(shown, "camera");
        if (!camera.empty())
            result.rows.emplace_back(changed ? tr("Catalog camera") : tr("Camera"), camera);
        const auto capture_date = text_value(shown, "capture_date");
        if (!capture_date.empty()) {
            result.rows.emplace_back(changed ? tr("Catalog capture date") : tr("Capture date"),
                                     capture_date);
            result.rows.emplace_back(tr("Date origin"),
                                     photo_label(text_value(shown, "date_origin")));
        }
        result.rows.emplace_back(tr("Albums"), text_value(shown, "albums"));
    }
    if (!selected.empty() && selected.size() == result.ids.size()) {
        const auto same = [&](std::string_view key) {
            const auto value = selected.front()->value(std::string{key}, Json{});
            return std::ranges::all_of(selected, [&](const Json* row) {
                return row->value(std::string{key}, Json{}) == value;
            });
        };
        if (same("description"))
            result.annotations.description = selected.front()->value("description", "");
        if (same("rating"))
            result.annotations.rating = selected.front()->value("rating", 0);
        if (same("date_override"))
            result.annotations.capture_date = text_value(*selected.front(), "date_override");
        if (same("tags"))
            result.annotations.tags = selected.front()->value("tags", std::vector<std::string>{});
    }
    return result;
}

void PhotoLibrary::save_annotations(std::vector<Id> ids, PhotoEdits edits,
                                    std::function<void(std::string)> done) {
    Json patch{{"op", "edit"}, {"ids", ids}};
    if (edits.description)
        patch["description"] = *edits.description;
    if (edits.rating)
        patch["rating"] = *edits.rating;
    if (edits.capture_date)
        patch["capture_date"] = edits.capture_date->empty() ? Json{} : Json(*edits.capture_date);
    if (!edits.add_tags.empty())
        patch["add_tags"] = edits.add_tags;
    if (!edits.remove_tags.empty())
        patch["remove_tags"] = edits.remove_tags;
    request(std::move(patch), [this, done = std::move(done)](Json response) {
        const auto error = response.value("ok", false)
                               ? std::string{}
                               : response.value("error", tr("Cannot save photo annotations"));
        if (error.empty()) {
            message_ = tr("Photo annotations saved");
            refresh();
        }
        done(error);
    });
}

void PhotoLibrary::create_album(Json ids, bool validated) {
    if (!catalog_identity_ready_)
        return;
    if (!ids.empty() && !validated) {
        const auto epoch = ++album_chooser_generation_;
        const auto form_generation = form_generation_;
        const auto dialog_generation = confirm_.generation();
        Json validation{{"op", "validate_selection"}, {"ids", ids}};
        request(std::move(validation), [this, ids = std::move(ids), epoch, form_generation,
                                        dialog_generation](Json response) {
            if (!open_ || epoch != album_chooser_generation_ ||
                form_generation != form_generation_ || dialog_generation != confirm_.generation() ||
                confirm_.is_open() || !response.value("ok", false))
                return;
            const auto missing = response.value("missing_ids", Json::array());
            if (!missing.empty()) {
                sync_selection();
                for (const auto& value : missing) {
                    const auto id = value.get<Id>();
                    selected_.erase(id);
                    selected_snapshots_.erase(id);
                    if (cursor_ == id)
                        cursor_ = 0;
                }
                focused_properties_.reset();
                selected_properties_.reset();
                publish();
                refresh();
                const auto detail = i18n::trn(
                    "{} selected photo no longer exists in the catalog. Review the "
                    "selection and create the album again.",
                    "{} selected photos no longer exist in the catalog. Review the "
                    "selection and create the album again.",
                    missing.size());
                report(detail);
                confirm_.open(tr("Review photo selection"), detail, tr("OK"), [](bool) {}, "");
                changed();
                return;
            }
            create_album(ids, true);
        });
        return;
    }
    name_album(std::move(ids));
}

void PhotoLibrary::name_album(Json ids, std::string initial_name, std::string initial_error) {
    const auto detail =
        !initial_error.empty() ? tr("Cannot create album: ") + initial_error
        : ids.empty()
            ? tr("Create an empty album. Originals stay in place.")
            : i18n::trn("Include {} selected photo. Originals stay in place.",
                        "Include {} selected photos. Originals stay in place.", ids.size());
    cancel_album_chooser();
    confirm_.open_name(
        tr("Create album"), detail, std::move(initial_name),
        [this, ids = std::move(ids)](const std::string& name) {
            if (name.empty())
                return std::string{tr("Enter a name")};
            const auto epoch = album_chooser_generation_;
            const auto form_generation = form_generation_;
            const auto dialog_generation = confirm_.generation();
            mutate({{"op", "create_album"}, {"name", name}, {"ids", ids}},
                   ids.empty() ? tr("Empty album created")
                               : i18n::trn("Album created with {} photo",
                                           "Album created with {} photos", ids.size()),
                   [this, ids, name, epoch, form_generation, dialog_generation](std::string error) {
                       if (open_ && epoch == album_chooser_generation_ &&
                           form_generation == form_generation_ &&
                           dialog_generation == confirm_.generation() && !confirm_.is_open())
                           name_album(ids, name, std::move(error));
                   });
            return std::string{};
        },
        tr("Create"));
    changed();
}

std::vector<MenuItem> PhotoLibrary::album_menu_items(Id id, std::string name) {
    if (id <= 0)
        return {};
    std::vector<MenuItem> items;
    MenuItem rename;
    rename.id = "photos:rename-album";
    rename.label = tr("Rename album…");
    rename.action = [this, alive = alive_, id, name] {
        if (!*alive)
            return;
        cancel_album_chooser();
        confirm_.open_name(
            tr("Rename album"), tr("Choose a new name for this album."), name,
            [this, id](const std::string& value) {
                if (value.empty())
                    return std::string{tr("Enter a name")};
                mutate({{"op", "rename_album"}, {"id", id}, {"name", value}}, tr("Album renamed"));
                return std::string{};
            },
            tr("Rename"));
        changed();
    };
    items.push_back(std::move(rename));
    MenuItem remove;
    remove.id = "photos:delete-album";
    remove.label = tr("Delete album…");
    remove.action = [this, alive = alive_, id, name] {
        if (!*alive)
            return;
        cancel_album_chooser();
        confirm_.open(
            tr("Delete album?"), name, tr("Delete album"),
            [this, id](bool accepted) {
                if (accepted)
                    mutate({{"op", "delete_album"}, {"id", id}}, tr("Album deleted; photos kept"));
            },
            tr("Only the album is deleted. Its photos remain in Photo Library and "
               "their original folders. This cannot be undone."));
        changed();
    };
    items.push_back(std::move(remove));
    MenuItem reset;
    reset.id = "photos:reset-album-cover";
    reset.label = tr("Use automatic album thumbnail");
    reset.action = [this, alive = alive_, id] {
        if (*alive)
            mutate({{"op", "set_album_cover"}, {"id", id}, {"photo_id", nullptr}},
                   tr("Automatic album thumbnail restored"));
    };
    items.push_back(std::move(reset));
    return items;
}

void PhotoLibrary::add_to_album() {
    const auto ids = selected_ids();
    if (ids.empty() || !choose_album_ || !photo_rows())
        return;
    const auto epoch = ++album_chooser_generation_;
    request({{"op", "albums"}}, [this, ids, epoch](Json response) {
        if (epoch != album_chooser_generation_ || !open_ || !photo_rows() ||
            !response.value("ok", false))
            return;
        std::vector<MenuItem> choices;
        for (const auto& album : response.value("albums", Json::array())) {
            const auto id = album.value("id", Id{});
            const auto name = album.value("name", "");
            MenuItem item;
            item.id = "photos:album-choice:" + std::to_string(id);
            item.label = name;
            item.action = [this, alive = alive_, ids, id, name, epoch] {
                if (!*alive || epoch != album_chooser_generation_ || !open_ || !photo_rows())
                    return;
                cancel_album_chooser();
                mutate({{"op", "edit"}, {"ids", ids}, {"add_albums", {id}}},
                       i18n::trn("Added {} photo to “{}”", "Added {} photos to “{}”", ids.size(),
                                 name));
            };
            choices.push_back(std::move(item));
        }
        MenuItem create;
        create.id = "photos:album-create";
        create.label = tr("Create new album…");
        create.action = [this, alive = alive_, ids, epoch] {
            if (!*alive || epoch != album_chooser_generation_ || !open_ || !photo_rows())
                return;
            create_album(ids);
        };
        choices.push_back(std::move(create));
        choose_album_(std::move(choices),
                      i18n::trn("Add {} photo to album…", "Add {} photos to album…", ids.size()));
    });
}

void PhotoLibrary::name_dialog(std::string title_text, std::string detail, std::string action,
                               std::function<void(std::string)> accept) {
    cancel_album_chooser();
    confirm_.open_name(
        std::move(title_text), std::move(detail), "",
        [accept = std::move(accept)](const std::string& name) {
            if (name.empty())
                return std::string{tr("Enter a name")};
            accept(name);
            return std::string{};
        },
        std::move(action));
    changed();
}

std::vector<MenuItem> PhotoLibrary::menu_items() {
    sync_selection();
    std::vector<MenuItem> result;
    const auto add = [&](std::string id, std::string label, std::function<void()> run,
                         bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.action = std::move(run);
        item.enabled = enabled;
        result.push_back(std::move(item));
    };
    add(
        "photos:add", tr("Add folder…"), [this] { add_folder(); },
        static_cast<bool>(choose_folder_));
    add("photos:filters", tr("Filter photos…"), [this] {
        open_form(Form::filters,
                  {{tr("Minimum rating (0–5)"), std::to_string(filter_.value("rating_min", 0))},
                   {tr("Capture date from (YYYY-MM-DD)"), filter_.value("date_from", "")},
                   {tr("Capture date to (YYYY-MM-DD)"), filter_.value("date_to", "")}});
    });
    add("photos:clear", tr("Clear photo filters"), [this] { show_all_photos(); });
    add("photos:retained", tr("Review retained photo associations"), [this] {
        clear_photo_selection();
        if (filter_.value("retained_only", false))
            filter_.erase("retained_only");
        else
            filter_["retained_only"] = true;
        search_.clear();
        active_saved_search_ = 0;
        offset_ = 0;
        show(Destination::library);
    });
    result.back().checkable = true;
    result.back().checked = filter_.value("retained_only", false);
    add("photos:save-search", tr("Save photo search…"), [this] {
        name_dialog(tr("Save photo search"), tr("Keep the current search and filters."), tr("Save"),
                    [this](std::string name) {
                        auto filter = filter_;
                        filter["text"] = search_;
                        mutate({{"op", "save_filter"}, {"name", name}, {"filter", filter}},
                               tr("Photo search saved"));
                    });
    });
    add(
        "photos:create-album", tr("Create album…"), [this] { create_album(selected_ids()); },
        catalog_identity_ready_);
    Json album = Json::object();
    if (destination_ == Destination::albums)
        album = focused_record();
    else if (destination_ == Destination::library && active_saved_search_ == 0 &&
             filter_.value("album_id", Id{}) > 0)
        album = {{"id", filter_["album_id"]}, {"name", collection_name_}};
    if (!album.empty()) {
        auto album_items = album_menu_items(album.value("id", Id{}), album.value("name", ""));
        for (auto& item : album_items)
            result.push_back(std::move(item));
        const auto ids = selected_ids();
        add(
            "photos:album-remove",
            photo_rows() ? tr("Remove from this album") : tr("Remove selected photos from album"),
            [this, id = album.value("id", Id{}), ids] {
                mutate({{"op", "edit"}, {"ids", ids}, {"remove_albums", {id}}},
                       tr("Album membership saved"));
            },
            !ids.empty());
        if (photo_rows()) {
            const auto member =
                ids.size() == 1 ? row_by_id_.find(ids[0].get<Id>()) : row_by_id_.end();
            const bool single_member = member != row_by_id_.end() &&
                                       rows_[member->second].value("availability", "") == "online";
            add(
                "photos:set-album-cover", tr("Use as album thumbnail"),
                [this, id = album.value("id", Id{}), ids] {
                    mutate({{"op", "set_album_cover"}, {"id", id}, {"photo_id", ids.at(0)}},
                           tr("Album thumbnail saved"));
                },
                single_member);
        }
    }

    if (photo_rows())
        add(
            "photos:add-to-album", tr("Add to album…"), [this] { add_to_album(); },
            !selected_.empty() && static_cast<bool>(choose_album_));
    if (photo_rows())
        add(
            "photos:edit", tr("Edit photo annotations…"),
            [this] {
                if (edit_)
                    edit_();
            },
            !selected_.empty() && static_cast<bool>(edit_));
    if (photo_rows()) {
        add("photos:select-page", tr("Select loaded photos"), [this] {
            folder_->select_all();
            sync_selection();
            changed();
        });
        add(
            "photos:clear-selection", tr("Clear all photo selection"),
            [this] {
                publishing_ = true;
                folder_->set_cursor(std::nullopt);
                publishing_ = false;
                selected_.clear();
                selected_snapshots_.clear();
                selected_properties_.reset();
                cursor_ = 0;
                selection_revision_ = folder_->selection_revision();
                changed();
            },
            !selected_.empty());
    }
    if (destination_ == Destination::review) {
        add("photos:find-duplicates", tr("Find exact duplicate candidates"),
            [this] { mutate({{"op", "find_duplicates"}}, tr("Duplicate review queued")); });
        add(
            "photos:duplicate-previous", tr("Previous duplicate groups"),
            [this] {
                review_offset_ = review_offset_ > page_size_ ? review_offset_ - page_size_ : 0;
                refresh();
            },
            review_offset_ > 0);
        add(
            "photos:duplicate-next", tr("Next duplicate groups"),
            [this] {
                review_offset_ += page_size_;
                refresh();
            },
            review_offset_ + records_.size() < review_total_);
    }
    if (destination_ == Destination::duplicate_group)
        add(
            "photos:trash-marked", tr("Move marked copies to Trash…"),
            [this] { confirm_duplicates(); }, !keepers_.empty() && keepers_.size() < rows_.size());
    add("photos:refresh", tr("Refresh Photo Library"),
        [this] { mutate({{"op", "refresh"}}, tr("Photo refresh queued")); });
    add(
        "photos:pause",
        background_paused_ ? tr("Resume background work") : tr("Pause background work"),
        [this] { control_background({{"paused", !background_paused_}}); }, background_ready_);
    result.back().separator = true;
    add(
        "photos:pace", tr("Gentle background pace"),
        [this] {
            control_background({{"pace", background_pace_ == "gentle" ? "normal" : "gentle"}});
        },
        background_ready_);
    result.back().checkable = true;
    result.back().checked = background_pace_ == "gentle";
    add("photos:export", tr("Export photo annotations…"), [this] { choose_annotations(true); });
    add("photos:restore", tr("Restore photo annotations…"), [this] { choose_annotations(false); });
    const auto group = [&](std::string id, std::string label,
                           std::initializer_list<std::string_view> actions) {
        MenuItem submenu;
        submenu.id = std::move(id);
        submenu.label = std::move(label);
        submenu.submenu = true;
        for (const auto action : actions) {
            const auto item = std::ranges::find(result, action, &MenuItem::id);
            if (item == result.end())
                continue;
            submenu.children.push_back(std::move(*item));
            result.erase(item);
        }
        if (!submenu.children.empty()) {
            submenu.children.front().separator = false;
            result.push_back(std::move(submenu));
        }
    };
    group("photos:search-menu", tr("Search and filters"),
          {"photos:filters", "photos:clear", "photos:save-search", "photos:retained"});
    group("photos:selection-menu", tr("Selection"),
          {"photos:add-to-album", "photos:edit", "photos:select-page", "photos:clear-selection"});
    group("photos:background-menu", tr("Background work"), {"photos:pause", "photos:pace"});
    group("photos:annotations-menu", tr("Annotations"), {"photos:export", "photos:restore"});
    return result;
}

std::vector<MenuItem> PhotoLibrary::context_menu_items() {
    sync_selection();
    std::vector<MenuItem> result;
    const auto add = [&](std::string id, std::string label, std::function<void()> run,
                         bool enabled = true) {
        if (!enabled)
            return;
        MenuItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.action = std::move(run);
        item.enabled = enabled;
        result.push_back(std::move(item));
    };
    const auto toolbar = menu_items();
    const auto append = [&](std::string_view id) {
        const auto find = [&](const auto& self,
                              const std::vector<MenuItem>& items) -> const MenuItem* {
            for (const auto& item : items) {
                if (item.id == id)
                    return &item;
                if (const auto* child = self(self, item.children))
                    return child;
            }
            return nullptr;
        };
        if (const auto* item = find(find, toolbar); item && item->enabled)
            result.push_back(*item);
    };
    const auto record = focused_record();
    const auto id = record.value("id", Id{});
    if (destination_ == Destination::sources && !record.empty()) {
        add("photos:show-source", tr("Show photos"),
            [this, id, record] { open_source(id, record.value("path", "")); });
        add(
            "photos:refresh-source", tr("Refresh folder"),
            [this, id] {
                mutate({{"op", "refresh"}, {"source_id", id}}, tr("Folder refresh queued"));
            },
            record.value("active", true));
        add(
            "photos:relink-source", tr("Relink source…"), [this, record] { relink_source(record); },
            static_cast<bool>(choose_folder_));
        add(
            "photos:inactive-source", tr("Make source inactive"),
            [this, id] {
                mutate({{"op", "remove_source"}, {"id", id}},
                       tr("Source inactive; originals and annotations retained"));
            },
            record.value("active", true));
        add(
            "photos:forget-source", tr("Forget source data…"),
            [this, id, record] {
                confirm_.open(
                    tr("Forget source catalog data?"), record.value("path", ""), tr("Forget data"),
                    [this, id](bool accepted) {
                        if (accepted)
                            mutate({{"op", "forget_source"}, {"id", id}},
                                   tr("Source catalog data forgotten"));
                    },
                    tr("Annotations and album memberships are removed. Originals stay unchanged."));
            },
            !record.value("active", true));
    } else if (destination_ == Destination::albums && !record.empty()) {
        add("photos:show-album", tr("Show album"),
            [this, id, record] { open_album(id, record.value("name", "")); });
        const auto ids = selected_ids();
        add(
            "photos:album-add", tr("Add selected photos to album"),
            [this, id, ids] {
                mutate({{"op", "edit"}, {"ids", ids}, {"add_albums", {id}}},
                       tr("Album membership saved"));
            },
            !ids.empty());
        append("photos:album-remove");
        for (auto& item : album_menu_items(id, record.value("name", "")))
            result.push_back(std::move(item));
    } else if (destination_ == Destination::filters && !record.empty()) {
        add("photos:run-search", tr("Find photos"), [this, record] { open_saved_search(record); });
    } else if (destination_ == Destination::review && !record.empty()) {
        add("photos:review-group", tr("Review these copies"),
            [this, record] { review_group(record); });
    } else if (destination_ == Destination::activity && !record.empty()) {
        if (record.value("file_operation", false)) {
            add("photos:operation-outcome", tr("Review operation outcome"), [this, record] {
                duplicate_group_ = record;
                open_form(Form::outcome);
            });
            add("photos:recover", tr("Recheck confirmed outcomes"),
                [this] { request({{"op", "recover_operations"}}, [this](Json) { refresh(); }); });
        } else {
            const auto state = record.value("state", "");
            for (const auto& [label, action] : std::array{
                     std::pair{tr("Pause job"), "pause"}, std::pair{tr("Resume job"), "resume"},
                     std::pair{tr("Cancel job"), "cancel"}, std::pair{tr("Retry job"), "retry"}}) {
                const bool enabled = std::string_view{action} == "pause"
                                         ? state == "running" || state == "queued"
                                     : std::string_view{action} == "resume" ? state == "paused"
                                     : std::string_view{action} == "retry"
                                         ? state == "failed" || state == "waiting"
                                         : state != "completed" && state != "cancelled";
                add(
                    "photos:job:" + std::string{action}, label,
                    [this, id, action] {
                        mutate({{"op", "job_control"}, {"id", id}, {"action", action}},
                               tr("Job updated at the next checkpoint"));
                    },
                    enabled);
            }
        }
    } else if (photo_rows()) {
        const auto focused = focused_photo();
        const std::filesystem::path focused_path{focused.value("path", "")};
        add(
            "photos:reveal", tr("Show in Folder"),
            [this, focused_path] {
                if (reveal_)
                    reveal_(focused_path);
            },
            !focused_path.empty() && static_cast<bool>(reveal_));
        const auto ids = selected_ids();
        if (ids.empty()) {
            result.clear();
            append("photos:select-page");
            return result;
        }
        append("photos:add-to-album");
        append("photos:album-remove");
        append("photos:set-album-cover");
        if (destination_ == Destination::duplicate_group) {
            add(
                "photos:keep", tr("Keep selected copies"),
                [this, ids] {
                    for (const auto& value : ids)
                        keepers_.insert(value.get<Id>());
                    ++data_revision_;
                    changed();
                },
                !ids.empty());
            add(
                "photos:mark-trash", tr("Mark selected copies for trash"),
                [this, ids] {
                    for (const auto& value : ids)
                        keepers_.erase(value.get<Id>());
                    ++data_revision_;
                    changed();
                },
                !ids.empty());
            add(
                "photos:trash-marked", tr("Move marked copies to Trash…"),
                [this] { confirm_duplicates(); },
                !keepers_.empty() && keepers_.size() < rows_.size());
        }
        const auto photos = selected_photos();
        const bool restorable = !ids.empty() && photos.size() == ids.size() &&
                                std::ranges::all_of(photos, [](const Json& row) {
                                    return row.value("availability", "") == "trashed";
                                });
        add(
            "photos:restore-originals", tr("Restore selected originals from trash"),
            [this, ids] {
                if (file_action_)
                    file_action_({{"kind", "restore"}, {"ids", ids}});
            },
            restorable && static_cast<bool>(file_action_));
    }
    if (record.empty() && destination_ == Destination::albums)
        append("photos:create-album");
    else if (record.empty() && destination_ == Destination::sources)
        append("photos:add");
    return result;
}

bool PhotoLibrary::activate() {
    if (photo_rows())
        return false;
    if (destination_ == Destination::activity && !focused_record().value("file_operation", false))
        return true;  // Job controls require an explicit context-menu choice.
    const auto items = context_menu_items();
    if (!focused_record().empty() && !items.empty() && items.front().enabled &&
        items.front().action)
        items.front().action();
    return true;
}

void PhotoLibrary::review_group(const Json& group) {
    sync_selection();
    if (group.value("photos", Json::array()).size() > 1000) {
        report(
            tr("This duplicate group exceeds the 1,000-file safety limit. Review smaller groups "
               "before moving files."));
        return;
    }
    duplicate_group_ = group;
    rows_ = group.value("photos", Json::array());
    total_ = rows_.size();
    keepers_.clear();
    keepers_.insert(group.value("keeper_id", Id{}));
    destination_ = Destination::duplicate_group;
    clear_photo_selection();
    publishing_ = true;
    folder_->replace_collection({}, title(), 0);
    publishing_ = false;
    selection_revision_ = folder_->selection_revision();
    publish();
}

void PhotoLibrary::confirm_duplicates() {
    Json ids = Json::array(), keep = Json::array();
    for (const auto& row : rows_) {
        const auto id = row.value("id", Id{});
        (keepers_.contains(id) ? keep : ids).push_back(id);
    }
    if (ids.empty() || keep.empty() || !file_action_)
        return;
    const Json reviewed = duplicate_group_.value("photos", Json::array());
    confirm_.open(
        i18n::trn("Move {} duplicate copy to Trash?", "Move {} duplicate copies to Trash?",
                  ids.size()),
        i18n::trn("Keep {} original. Recheck every file before moving.",
                  "Keep {} originals. Recheck every file before moving.", keep.size()),
        tr("Move to Trash"),
        [this, ids, keep, reviewed](bool accepted) {
            if (accepted)
                file_action_({{"kind", "trash"},
                              {"ids", ids},
                              {"keeper_ids", keep},
                              {"reviewed", reviewed}});
        },
        tr("Only marked copies move; kept originals stay in place."));
    changed();
}

void PhotoLibrary::add_folder(std::filesystem::path initial) {
    const auto review = [this](std::filesystem::path path) {
        source_path_ = std::move(path);
        recursive_ = true;
        open_form(Form::source, {{tr("Excluded subfolders (comma separated)"), ""}});
        form_detail_ = source_path_.string() + "\n" +
                       tr("Originals stay in place. Review the scope before indexing.");
    };
    if (!initial.empty()) {
        review(std::move(initial));
        return;
    }
    if (!choose_folder_) {
        report(tr("The folder chooser is unavailable"));
        return;
    }
    const auto generation = ++edit_generation_;
    choose_folder_({}, [this, alive = alive_, generation, review](std::filesystem::path path) {
        if (!*alive || generation != edit_generation_ || path.empty())
            return;
        review(std::move(path));
    });
}

void PhotoLibrary::relink_source(const Json& source) {
    if (!choose_folder_)
        return;
    const auto generation = ++edit_generation_;
    choose_folder_(source.value("path", ""), [this, alive = alive_, generation,
                                              source](std::filesystem::path path) {
        if (!*alive || generation != edit_generation_ || path.empty())
            return;
        source_id_ = source.value("id", Id{});
        source_path_ = std::move(path);
        open_form(Form::relink);
        form_detail_ = source_path_.string() + "\n" + tr("Checking photo identities…");
        const auto form_generation = form_generation_;
        request({{"op", "relink_source"},
                 {"source_id", source_id_},
                 {"path", source_path_.string()},
                 {"confirm", false}},
                [this, form_generation](Json result) {
                    if (form_generation != form_generation_ || !result.value("ok", false))
                        return;
                    const auto matched = result.value("matched", Json::array()).size();
                    const auto missing = result.value("missing", Json::array()).size();
                    const auto ambiguous = result.value("ambiguous", Json::array()).size();
                    const auto remounts = result.value("remount_candidates", Json::array()).size();
                    form_detail_ = source_path_.string() + "\n" +
                                   i18n::trf("Matched: {} · Missing: {} · Ambiguous: {}",
                                             matched + remounts, missing, ambiguous);
                    relink_remount_confirmation_ = result.value("requires_confirmation", false);
                    if (relink_remount_confirmation_) {
                        form_title_ = tr("Reconnect photo source");
                        form_detail_ +=
                            "\n\n" +
                            tr("This folder is present, but its device number has changed.") +
                            "\n" +
                            i18n::trf("{} photos match their recorded file details.",
                                      matched + remounts) +
                            "\n" + tr("Confirm this is the original folder to reconnect it.") +
                            "\n" + tr("Existing albums and annotations will be kept.");
                    }
                    relink_reviewed_ =
                        ambiguous == 0 &&
                        (relink_remount_confirmation_ ? remounts > 0 && missing == 0 : matched > 0);
                });
    });
}

void PhotoLibrary::open_form(Form form, std::vector<std::pair<std::string, std::string>> fields) {
    cancel_album_chooser();
    confirm_.close();
    ++edit_generation_;
    ++form_generation_;
    form_ = form;
    field_count_ = static_cast<int>(fields.size());
    focus_ = 0;
    form_message_.clear();
    form_detail_.clear();
    relink_reviewed_ = false;
    relink_remount_confirmation_ = false;
    controls_.clear();
    form_scroll_.jump_to(0);
    form_height_ = 0;
    for (std::size_t i = 0; i < fields.size() && i < fields_.size(); ++i) {
        labels_[i] = std::move(fields[i].first);
        fields_[i].set_text(std::move(fields[i].second));
    }
    switch (form) {
        case Form::source:
            form_title_ = tr("Add folder to Photo Library");
            break;
        case Form::filters:
            form_title_ = tr("Filter photos");
            break;
        case Form::relink:
            form_title_ = tr("Relink photo source");
            break;
        case Form::outcome:
            form_title_ = tr("Photo operation outcome");
            break;
        case Form::none:
            form_title_.clear();
            break;
    }
    changed();
}

void PhotoLibrary::close_form() {
    cancel_album_chooser();
    ++edit_generation_;
    ++form_generation_;
    form_ = Form::none;
    field_count_ = 0;
    controls_.clear();
    form_message_.clear();
    changed();
}

void PhotoLibrary::submit_form() {
    if (form_ == Form::source) {
        mutate({{"op", "add_source"},
                {"path", source_path_.string()},
                {"recursive", recursive_},
                {"exclusions", split(fields_[0].text())}},
               tr("Folder added. Indexing runs in the background."));
    } else if (form_ == Form::filters) {
        int rating = 0;
        if (!fields_[0].text().empty() &&
            (!number(fields_[0].text(), rating) || rating < 0 || rating > 5)) {
            form_message_ = tr("Enter a rating from 0 to 5");
            changed();
            return;
        }
        if (filter_.value("rating_min", 0) != rating ||
            filter_.value("date_from", "") != fields_[1].text() ||
            filter_.value("date_to", "") != fields_[2].text())
            clear_photo_selection();
        active_saved_search_ = 0;
        filter_["rating_min"] = rating;
        filter_["date_from"] = fields_[1].text();
        filter_["date_to"] = fields_[2].text();
        offset_ = 0;
        close_form();
        show(Destination::library);
    } else if (form_ == Form::relink && relink_reviewed_) {
        Json input{{"op", "relink_source"},
                   {"source_id", source_id_},
                   {"path", source_path_.string()},
                   {"confirm", true}};
        // This permission is granted only by submitting the explicit remount review above.
        // The backend revalidates the source and every candidate before accepting it.
        if (relink_remount_confirmation_)
            input["confirm_remount"] = true;
        mutate(std::move(input), tr("Source relinked; existing organisation retained"));
    }
}

void PhotoLibrary::choose_annotations(bool exporting) {
    if (!choose_path_) {
        report(tr("The file picker is not available"));
        return;
    }
    close_form();
    core::PickerRequest request;
    request.mode = exporting ? core::PickerRequest::Mode::save : core::PickerRequest::Mode::open;
    if (exporting)
        report(tr("Exports are limited to 256 MiB and 1,000,000 photos."));
    request.title = exporting ? tr("Export photo annotations") : tr("Restore photo annotations");
    request.accept_label = exporting ? tr("Export") : tr("Restore");
    request.name = exporting ? "rummage-photo-annotations.json" : "";
    request.filters = {{tr("JSON annotations"), {{0, "*.json"}}}};
    const auto generation = form_generation_;
    choose_path_(request, [this, alive = alive_, generation,
                           exporting](std::optional<std::filesystem::path> path) {
        if (!*alive || generation != form_generation_ || !path || path->empty())
            return;
        transfer_annotations(exporting, *path);
    });
}

void PhotoLibrary::transfer_annotations(bool exporting, const std::filesystem::path& path) {
    if (exporting) {
        const auto form_generation = form_generation_;
        request(
            {{"op", "export"}, {"encoded", true}}, [this, path, form_generation](Json response) {
                if (form_generation != form_generation_ || !response.value("ok", false))
                    return;
                auto data = response.at("json").get<std::string>();
                const auto task = ++task_ids_;
                pending_.emplace(
                    task, pool_.submit(
                              core::Priority::visible,
                              [path, data = std::move(data)](const core::CancelToken& token) {
                                  if (token.cancelled())
                                      return std::string{tr("Export cancelled")};
                                  if (const auto error = core::write_file_atomically(path, data))
                                      return std::string{tr("Cannot export annotations") + ": "} +
                                             error.message();
                                  return std::string{};
                              },
                              [this, alive = alive_, task, form_generation](std::string error) {
                                  if (!*alive)
                                      return;
                                  pending_.erase(task);
                                  if (error.empty()) {
                                      if (form_generation == form_generation_)
                                          close_form();
                                      report(tr("Photo annotations exported"));
                                  } else {
                                      if (form_generation == form_generation_)
                                          form_message_ = error;
                                      report(error);
                                  }
                              }));
            });
    } else {
        const auto form_generation = form_generation_;
        const auto task = ++task_ids_;
        pending_.emplace(
            task,
            pool_.submit(
                core::Priority::visible,
                [path](const core::CancelToken& token) {
                    try {
                        if (token.cancelled())
                            return Json{{"ok", false}, {"error", tr("Restore cancelled")}};
                        std::error_code error;
                        const auto size = std::filesystem::file_size(path, error);
                        if (error || size > 256ULL * 1024 * 1024)
                            return Json{{"ok", false},
                                        {"error", tr("Export unavailable or larger than 256 MiB")}};
                        std::ifstream input{path};
                        Json data;
                        input >> data;
                        return Json{{"ok", true}, {"data", data}};
                    } catch (const std::exception& error) {
                        return Json{{"ok", false}, {"error", error.what()}};
                    }
                },
                [this, alive = alive_, task, form_generation](Json result) {
                    if (!*alive)
                        return;
                    pending_.erase(task);
                    if (form_generation != form_generation_)
                        return;
                    if (!result.value("ok", false)) {
                        report(result.value("error", tr("Cannot read export")));
                        return;
                    }
                    confirm_.open(
                        tr("Restore photo annotations?"),
                        tr("For matching photos, replace description, rating and manual date; "
                           "merge tags and album memberships. Missing or ambiguous matches stay "
                           "unresolved. Originals are not moved."),
                        tr("Restore"),
                        [this, data = std::move(result["data"])](bool accepted) {
                            if (accepted)
                                mutate({{"op", "restore"}, {"data", data}},
                                       tr("Photo annotations restored"));
                        },
                        tr("New sources remain paused until you explicitly refresh them. Exports "
                           "and restores are limited to 256 MiB and 1,000,000 photos."));
                    changed();
                }));
    }
}

bool PhotoLibrary::has_dialog() const { return form_ != Form::none || confirm_.is_open(); }

bool PhotoLibrary::editing() const {
    return confirm_.is_open() || (form_ != Form::none && focus_ >= 0 && focus_ < field_count_);
}

void PhotoLibrary::paste(std::string_view text) {
    if (confirm_.is_open())
        confirm_.paste(text);
    else if (editing())
        fields_[static_cast<std::size_t>(focus_)].insert(text);
    changed();
}

std::string PhotoLibrary::copy_selection(bool cut) {
    if (confirm_.is_open())
        return confirm_.copy_selection(cut);
    if (!editing())
        return {};
    auto& field = fields_[static_cast<std::size_t>(focus_)];
    return cut ? field.cut() : field.selected_text();
}

bool PhotoLibrary::key_dialog(const ui::KeyEvent& event) {
    if (confirm_.is_open())
        return confirm_.key(event);
    if (form_ == Form::none)
        return false;
    if (event.keysym == XKB_KEY_Escape) {
        close_form();
        return true;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        ++edit_generation_;
        const int count = field_count_ + static_cast<int>(controls_.size());
        if (count > 0)
            focus_ =
                (focus_ +
                 (event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ? count - 1 : 1)) %
                count;
        if (focus_ < field_count_) {
            const auto rect = fields_[static_cast<std::size_t>(focus_)].bounds();
            if (rect.y < form_body_.y)
                form_scroll_.jump_to(form_scroll_.offset() - (form_body_.y - rect.y) - 20);
            else if (rect.bottom() > form_body_.bottom())
                form_scroll_.jump_to(form_scroll_.offset() + rect.bottom() - form_body_.bottom());
        }
    } else if (event.keysym == XKB_KEY_Page_Down || event.keysym == XKB_KEY_Page_Up) {
        form_scroll_.jump_to(form_scroll_.offset() +
                             (event.keysym == XKB_KEY_Page_Down ? 1.0F : -1.0F) *
                                 form_body_.height * 0.8F);
    } else if (focus_ >= 0 && focus_ < field_count_) {
        if (event.keysym == XKB_KEY_Return)
            submit_form();
        else
            fields_[static_cast<std::size_t>(focus_)].key(event);
    } else if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_space) {
        const auto index = focus_ - field_count_;
        if (index >= 0 && static_cast<std::size_t>(index) < controls_.size()) {
            const auto control = controls_[static_cast<std::size_t>(index)];
            if (control.enabled)
                control.run();
        } else
            submit_form();
    }
    changed();
    return true;
}

bool PhotoLibrary::pointer_dialog(const ui::PointerEvent& event) {
    if (confirm_.is_open()) {
        confirm_.pointer(event);
        return true;
    }
    if (form_ == Form::none)
        return false;
    if (event.kind == ui::PointerEvent::Kind::scroll && form_body_.contains(event.x, event.y)) {
        form_scroll_.jump_to(form_scroll_.offset() +
                             (event.wheel_y != 0 ? event.wheel_y * 40 : event.scroll_y));
        changed();
        return true;
    }
    if (event.kind == ui::PointerEvent::Kind::press && event.button == ui::PointerButton::left) {
        for (int i = 0; i < field_count_; ++i) {
            if (form_body_.contains(event.x, event.y) &&
                fields_[static_cast<std::size_t>(i)].bounds().contains(event.x, event.y)) {
                focus_ = i;
                ++edit_generation_;
                fields_[static_cast<std::size_t>(i)].pointer(event);
                changed();
                return true;
            }
        }
        for (std::size_t i = 0; i < controls_.size(); ++i) {
            const auto control = controls_[i];
            if (control.bounds.contains(event.x, event.y)) {
                focus_ = field_count_ + static_cast<int>(i);
                if (control.enabled)
                    control.run();
                changed();
                return true;
            }
        }
    } else if (editing())
        fields_[static_cast<std::size_t>(focus_)].pointer(event);
    changed();
    return true;
}

ui::CursorShape PhotoLibrary::cursor_at(float x, float y) const {
    if (confirm_.is_open())
        return confirm_.clickable_at(x, y) ? ui::CursorShape::pointer : ui::CursorShape::standard;
    for (int i = 0; i < field_count_; ++i)
        if (form_body_.contains(x, y) &&
            fields_[static_cast<std::size_t>(i)].bounds().contains(x, y))
            return ui::CursorShape::text;
    for (const auto& control : controls_)
        if (control.enabled && control.bounds.contains(x, y))
            return ui::CursorShape::pointer;
    return ui::CursorShape::standard;
}

ui::Rect PhotoLibrary::control_bounds(std::string_view label) const {
    for (const auto& control : controls_)
        if (control.label == label)
            return control.bounds;
    return {};
}

void PhotoLibrary::form_button(ui::Renderer& renderer, const theme::Palette& palette,
                               ui::Rect bounds, std::string label, std::function<void()> run,
                               bool enabled) {
    draw_dialog_button(renderer, bounds, palette, label,
                       focus_ == field_count_ + static_cast<int>(controls_.size()));
    if (!enabled)
        renderer.fill_rect(bounds, palette.surface.with_alpha(0.55F));
    controls_.push_back({std::move(label), bounds, std::move(run), enabled});
}

float PhotoLibrary::draw_wrapped(ui::Renderer& renderer, std::string_view text, float x, float y,
                                 float width, ui::Color color, float size) {
    float used = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        auto paragraph = text.substr(0, newline);
        do {
            std::size_t end = paragraph.size();
            while (end > 1 &&
                   renderer.fonts().layout(paragraph.substr(0, end), size, renderer.scale()).width /
                           renderer.scale() >
                       width) {
                --end;
                while (end > 0 && (static_cast<unsigned char>(paragraph[end]) & 0xC0U) == 0x80U)
                    --end;
            }
            if (end == 0)
                end = std::min<std::size_t>(paragraph.size(), 1);
            renderer.draw_text(paragraph.substr(0, end), x, y + used, size, color, width);
            used += size + 6;
            paragraph.remove_prefix(end);
        } while (!paragraph.empty());
        if (newline == std::string_view::npos)
            break;
        text.remove_prefix(newline + 1);
    }
    return used;
}

void PhotoLibrary::draw_dialogs(ui::Renderer& renderer, const ui::Rect& bounds,
                                const theme::Palette& palette) {
    if (confirm_.is_open()) {
        confirm_.draw(renderer, bounds, palette);
        return;
    }
    if (form_ == Form::none)
        return;
    const auto frame = DialogFrame::fit(bounds, 600, form_ == Form::filters ? 510.0F : 390.0F, 90);
    frame.draw(renderer, bounds, palette, form_title_);
    controls_.clear();
    form_body_ = frame.body;
    const float left = frame.body.x + 20, width = std::max(1.0F, frame.body.width - 40);
    form_scroll_.set_extent(form_height_, frame.body.height);
    float y = frame.body.y + 16 - form_scroll_.offset();
    const float start = y;
    renderer.push_clip(frame.body);
    if (!form_detail_.empty())
        y += draw_wrapped(renderer, form_detail_, left, y, width, palette.on_surface_variant) + 16;
    if (form_ == Form::outcome) {
        y += draw_wrapped(renderer,
                          tr("Status") + ": " + photo_label(duplicate_group_.value("state", "")) +
                              "\n" + duplicate_group_.value("error", ""),
                          left, y, width, palette.on_surface_variant) +
             12;
        for (const auto& row : duplicate_group_.value("photos", Json::array())) {
            y += draw_wrapped(renderer, row.value("path", ""), left, y, width, palette.on_surface) +
                 8;
            for (const auto& result : duplicate_group_.value("results", Json::array())) {
                if (result.value("id", Id{}) != row.value("id", Id{}))
                    continue;
                y += draw_wrapped(renderer,
                                  result.value("success", false)
                                      ? tr("Completed") + ": " + result.value("destination", "")
                                      : tr("Failed") + ": " + result.value("error", ""),
                                  left, y, width, palette.on_surface_variant) +
                     14;
            }
        }
    }
    for (int i = 0; i < field_count_; ++i) {
        const auto index = static_cast<std::size_t>(i);
        renderer.draw_text(labels_[index], left, y, 13, palette.on_surface_variant, width);
        fields_[index].draw(
            renderer, {left, y + 22, width, 34}, 14,
            {.background = palette.surface_container,
             .border = focus_ == i ? palette.primary : palette.outline_variant,
             .text = palette.on_surface,
             .selection = palette.primary_container,
             .caret = focus_ == i ? palette.primary : palette.primary.with_alpha(0)});
        y += 76;
    }
    const auto toggle = [&](std::string label, std::function<void()> run) {
        if (y + 34 > frame.body.y && y < frame.body.bottom()) {
            form_button(renderer, palette, {left, y, width, 34}, std::move(label), std::move(run));
            auto& hit = controls_.back().bounds;
            hit = hit.intersect(frame.body);
        }
        y += 44;
    };
    if (form_ == Form::source)
        toggle(recursive_ ? "✓ " + tr("Include subfolders") : tr("Include subfolders"),
               [this] { recursive_ = !recursive_; });
    if (form_ == Form::filters) {
        toggle(filter_.value("unknown_date", false) ? "✓ " + tr("Unknown capture date only")
                                                    : tr("Unknown capture date only"),
               [this] { filter_["unknown_date"] = !filter_.value("unknown_date", false); });
        toggle(filter_.value("include_inactive", false) ? "✓ " + tr("Include inactive sources")
                                                        : tr("Include inactive sources"),
               [this] { filter_["include_inactive"] = !filter_.value("include_inactive", false); });
    }
    form_height_ = y - start + 32;
    renderer.pop_clip();
    form_scroll_.set_extent(form_height_, frame.body.height);
    if (form_height_ > frame.body.height) {
        const float height = std::max(20.0F, frame.body.height * frame.body.height / form_height_);
        renderer.fill_rect({frame.body.right() - 5,
                            frame.body.y + (frame.body.height - height) * form_scroll_.offset() /
                                               std::max(1.0F, form_scroll_.max_offset()),
                            3, height},
                           palette.outline_variant);
    }
    const float footer_y = frame.footer.y + 10;
    form_button(renderer, palette, {frame.footer.right() - 224, footer_y, 96, 34},
                form_ == Form::outcome ? tr("Done") : tr("Cancel"), [this] { close_form(); });
    if (form_ != Form::outcome)
        form_button(
            renderer, palette, {frame.footer.right() - 116, footer_y, 96, 34},
            form_ == Form::source ? tr("Add folder")
            : form_ == Form::relink
                ? (relink_remount_confirmation_ ? tr("Reconnect") : tr("Relink"))
                : tr("Apply"),
            [this] { submit_form(); }, form_ != Form::relink || relink_reviewed_);
    renderer.draw_text(form_message_, frame.footer.x + 20, footer_y + 44, 12,
                       palette.on_surface_variant, frame.footer.width - 40);
}
}  // namespace rummage::views
