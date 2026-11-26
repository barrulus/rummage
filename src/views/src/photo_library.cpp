#include "rummage/views/photo_library.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/file.hpp"
#include "rummage/views/dialog_chrome.hpp"

namespace rummage::views {
namespace {
using Json = nlohmann::json;
using Id = PhotoLibrary::Id;

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
      confirm_{redraw_} {}

PhotoLibrary::~PhotoLibrary() {
    *alive_ = false;
    source_watcher_.reset();
    loop_.remove(work_timer_);
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
            return "Photo Library";
        case Destination::sources:
            return "Photo Sources";
        case Destination::albums:
            return "Photo Albums";
        case Destination::filters:
            return "Saved Photo Searches";
        case Destination::review:
            return "Review Duplicates";
        case Destination::duplicate_group:
            return "Duplicate Candidates";
        case Destination::activity:
            return "Photo Activity";
    }
    return "Photo Library";
}

std::string PhotoLibrary::status() const {
    sync_selection();
    std::string text;
    if (destination_ == Destination::duplicate_group)
        text = std::format("{} candidates · {} kept · {} marked for trash", rows_.size(),
                           keepers_.size(), rows_.size() - keepers_.size());
    else if (destination_ == Destination::library)
        text = std::format("{} photos · {} selected across pages · {} loaded · Page {}", total_,
                           selected_.size(), rows_.size(), offset_ / page_size_ + 1);
    else
        text = std::format("{} items", records_.size());
    if (querying_)
        text += " · Updating…";
    if (background_paused_)
        text += " · Background paused";
    if (!message_.empty())
        text += " · " + message_;
    else if (!progress_.empty())
        text += " · " + progress_;
    return text;
}

void PhotoLibrary::sync_selection() const {
    if (publishing_ || selection_revision_ == folder_->selection_revision())
        return;
    selection_revision_ = folder_->selection_revision();
    if (!photo_rows())
        return;
    for (const auto& row : rows_)
        selected_.erase(row.value("id", Id{}));
    for (const auto& key : folder_->selected_names())
        if (const auto id = photo_id(key); id > 0)
            selected_.insert(id);
    std::erase_if(selected_snapshots_,
                  [this](const auto& item) { return !selected_.contains(item.first); });
    for (const auto& row : rows_)
        if (selected_.contains(row.value("id", Id{})))
            selected_snapshots_[row.value("id", Id{})] = row;
    if (const auto* entry = folder_->cursor_entry())
        cursor_ = photo_id(entry->key());
    else
        cursor_ = 0;
    state_dirty_ = true;
}

const std::set<Id>& PhotoLibrary::selection() const {
    sync_selection();
    return selected_;
}

PhotoLibrary::Json PhotoLibrary::selected_ids() const {
    sync_selection();
    Json ids = Json::array();
    for (Id id : selected_)
        ids.push_back(id);
    return ids;
}

PhotoLibrary::Json PhotoLibrary::selected_photos() const {
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
    const auto found =
        std::ranges::find_if(rows_, [id](const Json& row) { return row.value("id", Id{}) == id; });
    return found == rows_.end() ? Json::object() : *found;
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
        cached_state_ = Json{{"version", 2},      {"text", search_},
                             {"filter", filter_}, {"selected", selected_ids()},
                             {"cursor", cursor_}, {"offset", offset_}}
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
        if (saved.value("version", 0) < 1 || saved.value("version", 0) > 2)
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
        const auto cursor = saved.value("cursor", Id{});
        if (offset < 0 || offset > 100000000 || cursor < 0)
            return;
        search_ = std::move(text);
        filter_ = std::move(filter);
        selected_ = std::move(selected);
        offset_ = static_cast<std::size_t>(offset);
        cursor_ = cursor;
        state_dirty_ = true;
    } catch (const Json::exception&) {
    }
}

void PhotoLibrary::restore_cursor(std::string_view key) {
    const auto id = photo_id(key);
    if (id <= 0)
        return;
    sync_selection();
    cursor_ = id;
    selected_.insert(id);
    selection_revision_ = folder_->selection_revision();
    state_dirty_ = true;
    if (!rows_.empty())
        publish();
}

void PhotoLibrary::open() {
    open_ = true;
    if (!client_->available()) {
        report(client_->error());
        return;
    }
    resume();
}

void PhotoLibrary::resume() {
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
                          message_ = response.value("error", "Photo Library request failed");
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
    entry.mime = mime_->by_name(entry.name).mime;
    entry.name_guess = entry.mime;
    entry.identity = "photo:" + std::to_string(row.value("id", Id{}));
    entry.location = row.value("path", "");
    entry.available = row.value("availability", "") == "online";
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

void PhotoLibrary::publish() {
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
            entry.identity = record_key(row, i);
            if (destination_ == Destination::sources)
                entry.name = row.value("path", "");
            else if (destination_ == Destination::albums || destination_ == Destination::filters)
                entry.name = row.value("name", "");
            else if (destination_ == Destination::review)
                entry.name = std::format("{} duplicate candidates",
                                         row.value("photos", Json::array()).size());
            else
                entry.name =
                    std::format("{} · {}", row.value("kind", "Job"), row.value("state", ""));
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

void PhotoLibrary::query() {
    sync_selection();
    query_job_.cancel();
    const auto generation = ++generation_;
    querying_ = true;
    Json input = filter_;
    input["op"] = "query";
    input["text"] = search_;
    input["offset"] = offset_;
    input["limit"] = page_size_;
    query_job_ = pool_.submit(
        core::Priority::visible,
        [client = client_, input](const core::CancelToken& token) {
            return client->request(input, &token);
        },
        [this, alive = alive_, generation](Json response) {
            if (!*alive || generation != generation_)
                return;
            querying_ = false;
            if (response.value("ok", false)) {
                sync_selection();
                rows_ = response.value("photos", Json::array());
                total_ = response.value("total", std::size_t{});
                publish();
            } else
                report(response.value("error", "Cannot query Photo Library"));
        });
    changed();
}

void PhotoLibrary::refresh() {
    if (destination_ == Destination::library) {
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
    request({{"op", op}}, [this, destination, generation, op](Json response) {
        if (generation != generation_ || destination != destination_)
            return;
        querying_ = false;
        if (!response.value("ok", false))
            return;
        records_ = response.value(op == "duplicates" ? "groups" : op, Json::array());
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

void PhotoLibrary::show(Destination destination) {
    sync_selection();
    query_job_.cancel();
    ++generation_;
    destination_ = destination;
    records_ = Json::array();
    close_form();
    confirm_.close();
    if (destination != Destination::library && destination != Destination::duplicate_group) {
        publishing_ = true;
        folder_->replace_collection({}, title(), 0, true);
        publishing_ = false;
        selection_revision_ = folder_->selection_revision();
    }
    refresh();
    changed();
}

void PhotoLibrary::set_query(std::string query_text) {
    if (query_text == search_)
        return;
    sync_selection();
    search_ = std::move(query_text);
    offset_ = 0;
    selected_.clear();
    selected_snapshots_.clear();
    cursor_ = 0;
    publishing_ = true;
    folder_->set_cursor(std::nullopt);
    publishing_ = false;
    selection_revision_ = folder_->selection_revision();
    if (destination_ != Destination::library)
        destination_ = Destination::library;
    query();
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

void PhotoLibrary::page(int direction) {
    sync_selection();
    if (direction < 0)
        offset_ = offset_ >= page_size_ ? offset_ - page_size_ : 0;
    else if (offset_ + rows_.size() < total_)
        offset_ += page_size_;
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
                } else if (response.value("worked", false)) {
                    if (source_watcher_)
                        source_watcher_->refresh_subscriptions();
                    if (open_)
                        refresh();
                    schedule_work();
                } else if (response.value("busy", false))
                    schedule_work(true);
            }
        },
        core::Priority::prefetch);
}

void PhotoLibrary::mutate(Json input, std::string success) {
    request(std::move(input), [this, form_generation = form_generation_,
                               success = std::move(success)](Json response) {
        if (!response.value("ok", false))
            return;
        message_ = success;
        if (response.contains("missing") || response.contains("ambiguous"))
            message_ = std::format("Restored: {} · Missing: {} · Ambiguous: {}",
                                   response.value("restored", 0),
                                   response.value("missing", Json::array()).size(),
                                   response.value("ambiguous", Json::array()).size());
        if (form_generation == form_generation_)
            close_form();
        if (source_watcher_)
            source_watcher_->refresh_subscriptions();
        refresh();
        schedule_work();
    });
}

void PhotoLibrary::refresh_progress() {
    const auto generation = ++progress_generation_;
    const auto background_generation = background_generation_;
    request({{"op", "jobs"}}, [this, generation, background_generation](Json response) {
        if (generation == progress_generation_ && response.value("ok", false)) {
            if (background_generation == background_generation_)
                update_background(response);
            update_progress(response.value("jobs", Json::array()));
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
                                         ? "Gentle pace uses smaller, less frequent batches."
                                         : "Normal background pace restored.")
                   : background_paused_
                       ? "Background work paused. In-flight items finish; no new items start."
                       : "Background work resumed. Individually paused jobs stay paused.";
        loop_.remove(work_timer_);
        work_timer_ = 0;
        refresh();
        schedule_work();
    });
}

void PhotoLibrary::update_progress(const Json& jobs) {
    if (jobs.empty()) {
        progress_ = "No background jobs. Add a folder when you are ready.";
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
    const auto title =
        shown->value("kind", "") == "duplicates" ? "Find exact duplicates" : "Index photos";
    progress_ = std::format("{}: {} · {} items completed", title, shown->value("state", ""),
                            shown->value("completed", 0));
    if (shown->contains("total") && !(*shown)["total"].is_null())
        progress_ += " / " + (*shown)["total"].dump();
    else
        progress_ += " · Total not yet known";
    const auto in_flight = shown->value("in_flight", 0);
    if (in_flight > 0)
        progress_ += std::format(" · {} in flight", in_flight);
    if (paused || waiting || failed)
        progress_ += std::format(" · {} paused · {} waiting · {} failed — see Activity", paused,
                                 waiting, failed);
}

PhotoProperties PhotoLibrary::properties(bool focused_only) const {
    PhotoProperties result;
    if (!photo_rows()) {
        const auto row = focused_record();
        const auto* entry = folder_->cursor_entry();
        result.title = entry ? entry->name : title();
        result.available = false;
        if (entry)
            result.entry = *entry;
        for (const auto& [key, label] :
             std::array{std::pair{"path", "Folder"}, std::pair{"availability", "Availability"},
                        std::pair{"recursive", "Include subfolders"},
                        std::pair{"exclusions", "Excluded folders"}, std::pair{"name", "Name"},
                        std::pair{"count", "Photos"}, std::pair{"kind", "Job"},
                        std::pair{"state", "State"}, std::pair{"completed", "Completed"},
                        std::pair{"total", "Total"}, std::pair{"error", "Error"},
                        std::pair{"reason", "Review reason"}}) {
            const auto value = std::string_view{key} == "recursive" && row.contains(key)
                                   ? std::string{row.value(key, false) ? "Yes" : "No"}
                                   : text_value(row, key);
            if (!value.empty())
                result.rows.emplace_back(label, value);
        }
        return result;
    }
    const auto focused = focused_photo();
    const auto ids =
        focused_only && focused.contains("id") ? Json::array({focused["id"]}) : selected_ids();
    result.ids = ids.get<std::vector<Id>>();
    Json selected = Json::array();
    for (const auto& row : rows_)
        if (std::ranges::find(result.ids, row.value("id", Id{})) != result.ids.end())
            selected.push_back(row);
    result.title = result.ids.size() == 1 && !selected.empty()
                       ? selected.front().value("filename", "")
                       : std::format("{} selected photos", result.ids.size());
    if (result.ids.empty())
        result.title = title();
    const auto shown = !focused.empty()   ? focused
                       : selected.empty() ? Json::object()
                                          : selected.front();
    if (!shown.empty()) {
        result.entry = photo_entry(shown);
        result.path = result.entry.location;
        result.available = result.entry.available;
        if (destination_ == Destination::duplicate_group)
            result.rows.emplace_back("Decision", keepers_.contains(shown.value("id", Id{}))
                                                     ? "Keep"
                                                     : "Marked for trash");
        result.rows.emplace_back("Availability", shown.value("availability", ""));
        result.rows.emplace_back("Location", shown.value("path", ""));
        result.rows.emplace_back("Dimensions", std::format("{} × {}", shown.value("width", 0),
                                                           shown.value("height", 0)));
        result.rows.emplace_back("Camera", text_value(shown, "camera"));
        result.rows.emplace_back("Capture date", text_value(shown, "capture_date"));
        result.rows.emplace_back("Date origin", text_value(shown, "date_origin"));
        result.rows.emplace_back("Albums", text_value(shown, "albums"));
    }
    if (!selected.empty() && selected.size() == result.ids.size()) {
        const auto same = [&](std::string_view key) {
            const auto value = selected.front().value(std::string{key}, Json{});
            return std::ranges::all_of(selected, [&](const Json& row) {
                return row.value(std::string{key}, Json{}) == value;
            });
        };
        if (same("description"))
            result.annotations.description = selected.front().value("description", "");
        if (same("rating"))
            result.annotations.rating = selected.front().value("rating", 0);
        if (same("date_override"))
            result.annotations.capture_date = text_value(selected.front(), "date_override");
        if (same("tags"))
            result.annotations.tags = selected.front().value("tags", std::vector<std::string>{});
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
                               : response.value("error", "Cannot save photo annotations");
        if (error.empty()) {
            message_ = "Photo annotations saved";
            refresh();
        }
        done(error);
    });
}

void PhotoLibrary::name_dialog(std::string title_text, std::string detail, std::string action,
                               std::function<void(std::string)> accept) {
    confirm_.open_name(
        std::move(title_text), std::move(detail), "",
        [accept = std::move(accept)](const std::string& name) {
            if (name.empty())
                return std::string{"Enter a name"};
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
    for (const auto& [label, destination] : std::array{
             std::pair{"Library", Destination::library}, std::pair{"Sources", Destination::sources},
             std::pair{"Albums", Destination::albums},
             std::pair{"Saved searches", Destination::filters},
             std::pair{"Review duplicates", Destination::review},
             std::pair{"Activity", Destination::activity}}) {
        add("photos:destination:" + std::to_string(static_cast<int>(destination)), label,
            [this, destination] { show(destination); });
        result.back().radio = result.back().checkable = true;
        result.back().checked = destination_ == destination;
    }
    add("photos:add", "Add folder…", [this] { add_folder(); }, static_cast<bool>(choose_folder_));
    result.back().separator = true;
    add("photos:filters", "Filter photos…", [this] {
        open_form(Form::filters,
                  {{"Minimum rating (0–5)", std::to_string(filter_.value("rating_min", 0))},
                   {"Capture date from (YYYY-MM-DD)", filter_.value("date_from", "")},
                   {"Capture date to (YYYY-MM-DD)", filter_.value("date_to", "")}});
    });
    add("photos:clear", "Clear photo filters", [this] {
        filter_ = Json::object();
        set_query("");
        offset_ = 0;
        show(Destination::library);
    });
    add("photos:save-search", "Save photo search…", [this] {
        name_dialog("Save photo search", "Keep the current search and filters.", "Save",
                    [this](std::string name) {
                        auto filter = filter_;
                        filter["text"] = search_;
                        mutate({{"op", "save_filter"}, {"name", name}, {"filter", filter}},
                               "Photo search saved");
                    });
    });
    add("photos:create-album", "Create album…", [this] {
        name_dialog("Create album", "Albums organise photos without moving originals.", "Create",
                    [this](std::string name) {
                        mutate({{"op", "create_album"}, {"name", name}}, "Album created");
                    });
    });
    if (photo_rows())
        add(
            "photos:edit", "Edit photo annotations…",
            [this] {
                if (edit_)
                    edit_();
            },
            !selected_.empty() && static_cast<bool>(edit_));
    if (photo_rows()) {
        add("photos:select-page", "Select loaded photos (this page)", [this] {
            folder_->select_all();
            sync_selection();
            changed();
        });
        add(
            "photos:clear-selection", "Clear all photo selection",
            [this] {
                publishing_ = true;
                folder_->set_cursor(std::nullopt);
                publishing_ = false;
                selected_.clear();
                selected_snapshots_.clear();
                cursor_ = 0;
                selection_revision_ = folder_->selection_revision();
                changed();
            },
            !selected_.empty());
    }
    if (destination_ == Destination::library) {
        add("photos:previous", "Previous photo page", [this] { page(-1); }, offset_ > 0);
        add("photos:next", "Next photo page", [this] { page(1); }, offset_ + rows_.size() < total_);
    }
    if (destination_ == Destination::review)
        add("photos:find-duplicates", "Find exact duplicate candidates",
            [this] { mutate({{"op", "find_duplicates"}}, "Duplicate review queued"); });
    if (destination_ == Destination::duplicate_group)
        add(
            "photos:trash-marked", "Move marked copies to Trash…", [this] { confirm_duplicates(); },
            !keepers_.empty() && keepers_.size() < rows_.size());
    add("photos:refresh", "Refresh Photo Library",
        [this] { mutate({{"op", "refresh"}}, "Photo refresh queued"); });
    add(
        "photos:pause", background_paused_ ? "Resume background work" : "Pause background work",
        [this] { control_background({{"paused", !background_paused_}}); }, background_ready_);
    result.back().separator = true;
    add(
        "photos:pace", "Gentle background pace",
        [this] {
            control_background({{"pace", background_pace_ == "gentle" ? "normal" : "gentle"}});
        },
        background_ready_);
    result.back().checkable = true;
    result.back().checked = background_pace_ == "gentle";
    add("photos:export", "Export photo annotations…", [this] { choose_annotations(true); });
    add("photos:restore", "Restore photo annotations…", [this] { choose_annotations(false); });
    return result;
}

std::vector<MenuItem> PhotoLibrary::context_menu_items() {
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
    const auto record = focused_record();
    const auto id = record.value("id", Id{});
    if (destination_ == Destination::sources && !record.empty()) {
        add("photos:show-source", "Show photos", [this, id] {
            filter_["source_id"] = id;
            offset_ = 0;
            show(Destination::library);
        });
        add("photos:refresh-source", "Refresh folder", [this, id] {
            mutate({{"op", "refresh"}, {"source_id", id}}, "Folder refresh queued");
        });
        add(
            "photos:relink-source", "Relink source…", [this, record] { relink_source(record); },
            static_cast<bool>(choose_folder_));
        add(
            "photos:inactive-source", "Make source inactive",
            [this, id] {
                mutate({{"op", "remove_source"}, {"id", id}},
                       "Source inactive; originals and annotations retained");
            },
            record.value("active", true));
        add(
            "photos:forget-source", "Forget source data…",
            [this, id, record] {
                confirm_.open(
                    "Forget source catalog data?", record.value("path", ""), "Forget data",
                    [this, id](bool accepted) {
                        if (accepted)
                            mutate({{"op", "forget_source"}, {"id", id}},
                                   "Source catalog data forgotten");
                    },
                    "Annotations and album memberships are removed. Originals stay unchanged.");
            },
            !record.value("active", true));
    } else if (destination_ == Destination::albums && !record.empty()) {
        add("photos:show-album", "Show album", [this, id] {
            filter_["album_id"] = id;
            offset_ = 0;
            show(Destination::library);
        });
        const auto ids = selected_ids();
        add(
            "photos:album-add", "Add selected photos to album",
            [this, id, ids] {
                mutate({{"op", "edit"}, {"ids", ids}, {"add_albums", {id}}},
                       "Album membership saved");
            },
            !ids.empty());
        add(
            "photos:album-remove", "Remove selected photos from album",
            [this, id, ids] {
                mutate({{"op", "edit"}, {"ids", ids}, {"remove_albums", {id}}},
                       "Album membership saved");
            },
            !ids.empty());
    } else if (destination_ == Destination::filters && !record.empty()) {
        add("photos:run-search", "Find photos", [this, record] {
            filter_ = record.value("filter", Json::object());
            search_ = filter_.value("text", "");
            offset_ = 0;
            show(Destination::library);
        });
    } else if (destination_ == Destination::review && !record.empty()) {
        add("photos:review-group", "Review these copies", [this, record] { review_group(record); });
    } else if (destination_ == Destination::activity && !record.empty()) {
        if (record.value("file_operation", false)) {
            add("photos:operation-outcome", "Review operation outcome", [this, record] {
                duplicate_group_ = record;
                open_form(Form::outcome);
            });
            add("photos:recover", "Recheck confirmed outcomes",
                [this] { request({{"op", "recover_operations"}}, [this](Json) { refresh(); }); });
        } else {
            const auto state = record.value("state", "");
            for (const auto& [label, action] :
                 std::array{std::pair{"Pause job", "pause"}, std::pair{"Resume job", "resume"},
                            std::pair{"Cancel job", "cancel"}, std::pair{"Retry job", "retry"}}) {
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
                               "Job updated at the next checkpoint");
                    },
                    enabled);
            }
        }
    } else if (photo_rows()) {
        const auto focused = focused_photo();
        const std::filesystem::path focused_path{focused.value("path", "")};
        add(
            "photos:reveal", "Show in Folder",
            [this, focused_path] {
                if (reveal_)
                    reveal_(focused_path);
            },
            !focused_path.empty() && static_cast<bool>(reveal_));
        const auto ids = selected_ids();
        add(
            "photos:edit", "Edit photo annotations…",
            [this] {
                if (edit_)
                    edit_();
            },
            !ids.empty() && static_cast<bool>(edit_));
        if (destination_ == Destination::duplicate_group) {
            add(
                "photos:keep", "Keep selected copies",
                [this, ids] {
                    for (const auto& value : ids)
                        keepers_.insert(value.get<Id>());
                    changed();
                },
                !ids.empty());
            add(
                "photos:mark-trash", "Mark selected copies for trash",
                [this, ids] {
                    for (const auto& value : ids)
                        keepers_.erase(value.get<Id>());
                    changed();
                },
                !ids.empty());
            add(
                "photos:trash-marked", "Move marked copies to Trash…",
                [this] { confirm_duplicates(); },
                !keepers_.empty() && keepers_.size() < rows_.size());
        }
        const auto photos = selected_photos();
        const bool restorable = !ids.empty() && photos.size() == ids.size() &&
                                std::ranges::all_of(photos, [](const Json& row) {
                                    return row.value("availability", "") == "trashed";
                                });
        add(
            "photos:restore-originals", "Restore selected originals from trash",
            [this, ids] {
                if (file_action_)
                    file_action_({{"kind", "restore"}, {"ids", ids}});
            },
            restorable && static_cast<bool>(file_action_));
    }
    auto common = menu_items();
    if (!common.empty())
        common.front().separator = true;
    for (auto& item : common)
        result.push_back(std::move(item));
    return result;
}

bool PhotoLibrary::activate() {
    if (photo_rows())
        return false;
    const auto items = context_menu_items();
    if (!focused_record().empty() && !items.empty() && items.front().enabled &&
        items.front().action)
        items.front().action();
    return true;
}

void PhotoLibrary::review_group(const Json& group) {
    sync_selection();
    duplicate_group_ = group;
    rows_ = group.value("photos", Json::array());
    total_ = rows_.size();
    keepers_.clear();
    keepers_.insert(group.value("keeper_id", Id{}));
    destination_ = Destination::duplicate_group;
    selected_.clear();
    cursor_ = 0;
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
        std::format("Move {} duplicate copies to Trash?", ids.size()),
        std::format("Keep {} originals. Recheck every file before moving.", keep.size()),
        "Move to Trash",
        [this, ids, keep, reviewed](bool accepted) {
            if (accepted)
                file_action_({{"kind", "trash"},
                              {"ids", ids},
                              {"keeper_ids", keep},
                              {"reviewed", reviewed}});
        },
        "Only marked copies move; kept originals stay in place.");
    changed();
}

void PhotoLibrary::add_folder(std::filesystem::path initial) {
    const auto review = [this](std::filesystem::path path) {
        source_path_ = std::move(path);
        recursive_ = true;
        open_form(Form::source, {{"Excluded subfolders (comma separated)", ""}});
        form_detail_ =
            source_path_.string() + "\nOriginals stay in place. Review the scope before indexing.";
    };
    if (!initial.empty()) {
        review(std::move(initial));
        return;
    }
    if (!choose_folder_) {
        report("The folder chooser is unavailable");
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
        relink_reviewed_ = false;
        open_form(Form::relink);
        form_detail_ = source_path_.string() + "\nChecking photo identities…";
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
                    form_detail_ = source_path_.string() +
                                   std::format("\nMatched: {} · Missing: {} · Ambiguous: {}",
                                               matched, missing, ambiguous);
                    relink_reviewed_ = matched > 0 && ambiguous == 0;
                });
    });
}

void PhotoLibrary::open_form(Form form, std::vector<std::pair<std::string, std::string>> fields) {
    confirm_.close();
    ++edit_generation_;
    ++form_generation_;
    form_ = form;
    field_count_ = static_cast<int>(fields.size());
    focus_ = 0;
    form_message_.clear();
    form_detail_.clear();
    controls_.clear();
    form_scroll_.jump_to(0);
    form_height_ = 0;
    for (std::size_t i = 0; i < fields.size() && i < fields_.size(); ++i) {
        labels_[i] = std::move(fields[i].first);
        fields_[i].set_text(std::move(fields[i].second));
    }
    switch (form) {
        case Form::source:
            form_title_ = "Add folder to Photo Library";
            break;
        case Form::filters:
            form_title_ = "Filter photos";
            break;
        case Form::relink:
            form_title_ = "Relink photo source";
            break;
        case Form::outcome:
            form_title_ = "Photo operation outcome";
            break;
        case Form::none:
            form_title_.clear();
            break;
    }
    changed();
}

void PhotoLibrary::close_form() {
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
               "Folder added. Indexing runs in the background.");
    } else if (form_ == Form::filters) {
        int rating = 0;
        if (!fields_[0].text().empty() &&
            (!number(fields_[0].text(), rating) || rating < 0 || rating > 5)) {
            form_message_ = "Enter a rating from 0 to 5";
            changed();
            return;
        }
        filter_["rating_min"] = rating;
        filter_["date_from"] = fields_[1].text();
        filter_["date_to"] = fields_[2].text();
        offset_ = 0;
        close_form();
        show(Destination::library);
    } else if (form_ == Form::relink && relink_reviewed_) {
        mutate({{"op", "relink_source"},
                {"source_id", source_id_},
                {"path", source_path_.string()},
                {"confirm", true}},
               "Source relinked; existing organisation retained");
    }
}

void PhotoLibrary::choose_annotations(bool exporting) {
    if (!choose_path_) {
        report("The file picker is not available");
        return;
    }
    close_form();
    core::PickerRequest request;
    request.mode = exporting ? core::PickerRequest::Mode::save : core::PickerRequest::Mode::open;
    request.title = exporting ? "Export photo annotations" : "Restore photo annotations";
    request.accept_label = exporting ? "Export" : "Restore";
    request.name = exporting ? "rummage-photo-annotations.json" : "";
    request.filters = {{"JSON annotations", {{0, "*.json"}}}};
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
        request({{"op", "export"}}, [this, path, form_generation](Json response) {
            if (form_generation != form_generation_ || !response.value("ok", false))
                return;
            const auto data = response.at("data").dump(2);
            const auto task = ++task_ids_;
            pending_.emplace(task,
                             pool_.submit(
                                 core::Priority::visible,
                                 [path, data](const core::CancelToken& token) {
                                     if (token.cancelled())
                                         return std::string{"Export cancelled"};
                                     if (const auto error = core::write_file_atomically(path, data))
                                         return std::string{"Cannot export annotations: "} +
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
                                         report("Photo annotations exported");
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
            task, pool_.submit(
                      core::Priority::visible,
                      [path](const core::CancelToken& token) {
                          try {
                              if (token.cancelled())
                                  return Json{{"ok", false}, {"error", "Restore cancelled"}};
                              std::error_code error;
                              const auto size = std::filesystem::file_size(path, error);
                              if (error || size > 256ULL * 1024 * 1024)
                                  return Json{
                                      {"ok", false},
                                      {"error", "Export unavailable or larger than 256 MiB"}};
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
                              report(result.value("error", "Cannot read export"));
                              return;
                          }
                          mutate({{"op", "restore"}, {"data", result["data"]}},
                                 "Photo annotations restored");
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
                          "Status: " + duplicate_group_.value("state", "") + "\n" +
                              duplicate_group_.value("error", ""),
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
                                      ? "Completed: " + result.value("destination", "")
                                      : "Failed: " + result.value("error", ""),
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
        toggle(recursive_ ? "✓ Include subfolders" : "Include subfolders",
               [this] { recursive_ = !recursive_; });
    if (form_ == Form::filters) {
        toggle(filter_.value("unknown_date", false) ? "✓ Unknown capture date only"
                                                    : "Unknown capture date only",
               [this] { filter_["unknown_date"] = !filter_.value("unknown_date", false); });
        toggle(filter_.value("include_inactive", false) ? "✓ Include inactive sources"
                                                        : "Include inactive sources",
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
                form_ == Form::outcome ? "Done" : "Cancel", [this] { close_form(); });
    if (form_ != Form::outcome)
        form_button(
            renderer, palette, {frame.footer.right() - 116, footer_y, 96, 34},
            form_ == Form::source   ? "Add folder"
            : form_ == Form::relink ? "Relink"
                                    : "Apply",
            [this] { submit_form(); }, form_ != Form::relink || relink_reviewed_);
    renderer.draw_text(form_message_, frame.footer.x + 20, footer_y + 44, 12,
                       palette.on_surface_variant, frame.footer.width - 40);
}
}  // namespace rummage::views
