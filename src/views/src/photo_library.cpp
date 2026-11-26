#include "rummage/views/photo_library.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {
using Json = nlohmann::json;

std::vector<std::string> split(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(',', start);
        if (end == std::string_view::npos)
            end = value.size();
        auto token = value.substr(start, end - start);
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            const auto last = token.find_last_not_of(" \t\r\n");
            result.emplace_back(token.substr(first, last - first + 1));
        }
        start = end + 1;
    }
    return result;
}

std::string joined(const Json& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty())
            result += ", ";
        result += value.is_string() ? value.get<std::string>() : value.value("name", "");
    }
    return result;
}

std::string text_value(const Json& row, std::string_view key) {
    const auto found = row.find(std::string{key});
    if (found == row.end() || found->is_null())
        return {};
    if (found->is_string())
        return found->get<std::string>();
    if (found->is_array())
        return joined(*found);
    return found->dump();
}

bool number(std::string_view text, int& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}  // namespace

PhotoLibrary::PhotoLibrary(core::EventLoop& loop, core::WorkerPool& pool, Thumbnails* thumbnails,
                           std::function<void()> redraw,
                           std::function<void(Action, std::filesystem::path)> action,
                           fs::DirectoryWatcher* watcher)
    : loop_{loop},
      pool_{pool},
      thumbnails_{thumbnails},
      redraw_{std::move(redraw)},
      action_{std::move(action)},
      client_{std::make_shared<photos::Client>()},
      watcher_{watcher} {}

PhotoLibrary::~PhotoLibrary() {
    *alive_ = false;
    source_watcher_.reset();
    loop_.remove(work_timer_);
    query_job_.cancel();
    for (const auto& [id, token] : pending_)
        token.cancel();
}

const std::string& PhotoLibrary::state() const {
    if (!state_dirty_)
        return cached_state_;
    cached_state_ = Json{
        {"version", 1},
        {"text", search_.text()},
        {"filter", filter_},
        {"selected", selected_ids()},
        {"cursor", cursor_},
        {"anchor", anchor_},
        {"offset", offset_},
        {"first", first_},
        {"first_item", scroll_anchor_.value_or(first_ * columns_)},
        {"grid", grid_}}.dump();
    state_dirty_ = false;
    return cached_state_;
}

void PhotoLibrary::restore_state(std::string_view state) {
    if (state.empty() || state.size() > 4 * 1024 * 1024)
        return;
    try {
        const auto saved = Json::parse(state);
        if (saved.value("version", 0) != 1)
            return;
        auto text = saved.value("text", std::string{});
        if (text.size() > 16384)
            return;
        auto filter = saved.value("filter", Json::object());
        if (!filter.is_object())
            return;
        std::set<Id> selection;
        const auto selected = saved.value("selected", Json::array());
        if (!selected.is_array() || selected.size() > 100000)
            return;
        for (const auto& id : selected) {
            if (!id.is_number_integer() || id.get<Id>() <= 0)
                return;
            selection.insert(id.get<Id>());
        }
        const auto cursor = saved.value("cursor", Id{});
        const auto anchor = saved.value("anchor", Id{});
        const auto offset = saved.value("offset", std::int64_t{});
        const auto first = saved.value("first", std::int64_t{});
        const auto grid = saved.value("grid", true);
        if (cursor < 0 || anchor < 0 || offset < 0 || offset > 100000000 || first < 0 ||
            first > 200)
            return;
        search_.set_text(std::move(text));
        filter_ = std::move(filter);
        selected_ = std::move(selection);
        cursor_ = cursor;
        anchor_ = anchor;
        offset_ = static_cast<std::size_t>(offset);
        first_ = static_cast<std::size_t>(first);
        grid_ = grid;
        scroll_anchor_ = saved.value("first_item", static_cast<std::size_t>(first));
        state_dirty_ = true;
        ++edit_generation_;
    } catch (const Json::exception&) {
        // A newer or damaged workspace state cannot prevent ordinary file browsing.
    }
}

void PhotoLibrary::open() {
    open_ = true;
    if (!client_->available()) {
        message_ = client_->error();
        redraw_();
        return;
    }
    resume();
}

void PhotoLibrary::resume() {
    if (watcher_ && !source_watcher_)
        source_watcher_ = std::make_unique<photos::SourceWatcher>(loop_, pool_, *watcher_, client_,
                                                                  [this](const std::string& error) {
                                                                      if (!error.empty())
                                                                          report(error);
                                                                      else {
                                                                          if (open_)
                                                                              refresh();
                                                                          schedule_work();
                                                                      }
                                                                  });
    if (source_watcher_)
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
    redraw_();
    // Named jobs continue while the owning browser is alive. Closing the final window stops
    // submitting batches; unfinished work remains durable for the next explicit library open.
}

void PhotoLibrary::request(Json input, std::function<void(Json)> done, core::Priority priority) {
    if (!client_->available()) {
        message_ = client_->error();
        redraw_();
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
                          message_ =
                              response.value("error", std::string{"Photo Library request failed"});
                          form_message_ = message_;
                      }
                      done(std::move(response));
                      redraw_();
                  }));
}

void PhotoLibrary::search_changed() {
    destination_ = Destination::library;
    offset_ = first_ = 0;
    scroll_anchor_.reset();
    selected_.clear();
    cursor_ = anchor_ = 0;
    query();
}

void PhotoLibrary::query() {
    query_job_.cancel();
    const auto generation = ++generation_;
    querying_ = true;
    Json input = filter_;
    input["op"] = "query";
    input["text"] = search_.text();
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
                state_dirty_ = true;
                rows_ = response.value("photos", Json::array());
                total_ = response.value("total", std::size_t{});
                // Keep captured selection identities across arriving rows and pages. An absent
                // cursor is not silently retargeted to a different photograph.
                if (!cursor_ && !rows_.empty())
                    cursor_ = rows_.front().value("id", Id{});
                first_ = std::min(first_, rows_.empty() ? 0 : rows_.size() - 1);
            } else
                message_ = response.value("error", std::string{"Cannot query the photo catalog"});
            redraw_();
        });
}

void PhotoLibrary::refresh() {
    if (destination_ == Destination::library) {
        query();
        refresh_progress();
        return;
    }
    const auto destination = destination_;
    const std::string op = destination == Destination::activity  ? "jobs"
                           : destination == Destination::sources ? "sources"
                           : destination == Destination::albums  ? "albums"
                           : destination == Destination::filters ? "filters"
                                                                 : "duplicates";
    const auto generation = ++records_generation_;
    request({{"op", op}}, [this, destination, op, generation](Json response) {
        if (destination_ != destination || generation != records_generation_ ||
            !response.value("ok", false))
            return;
        records_ = response.value(op == "duplicates" ? "groups" : op, Json::array());
        if (destination == Destination::activity) {
            update_background(response);
            update_progress(records_);
        }
        if (destination == Destination::activity)
            request({{"op", "operations"}}, [this, generation](Json operations) {
                if (destination_ != Destination::activity || generation != records_generation_ ||
                    !operations.value("ok", false))
                    return;
                for (auto operation : operations.value("operations", Json::array())) {
                    operation["file_operation"] = true;
                    records_.push_back(std::move(operation));
                }
            });
    });
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

void PhotoLibrary::switch_to(Destination destination) {
    close_form();
    destination_ = destination;
    records_ = Json::array();
    records_first_ = 0;
    focus_ = -1;
    refresh();
}

void PhotoLibrary::open_form(Form form, std::vector<std::pair<std::string, std::string>> fields) {
    ++edit_generation_;
    ++form_generation_;
    form_ = form;
    form_scroll_ = form_content_height_ = 0;
    wrapped_paths_.clear();
    field_count_ = static_cast<int>(std::min(fields.size(), fields_.size()));
    for (std::size_t i = 0; i < fields.size() && i < fields_.size(); ++i) {
        labels_[i] = std::move(fields[i].first);
        fields_[i].set_text(std::move(fields[i].second));
    }
    focus_ = 0;
    form_message_.clear();
    redraw_();
}

void PhotoLibrary::close_form() {
    ++edit_generation_;
    ++form_generation_;
    form_ = Form::none;
    field_count_ = 1;
    focus_ = -1;
    form_message_.clear();
    redraw_();
}

void PhotoLibrary::add_folder(std::filesystem::path initial) {
    open_form(Form::source, {{"Folder (absolute path)", initial.string()},
                             {"Excluded subfolders (comma separated)", ""}});
    recursive_ = true;
}

PhotoLibrary::Json PhotoLibrary::selected_ids() const {
    Json ids = Json::array();
    for (const auto id : selected_)
        ids.push_back(id);
    return ids;
}

std::string PhotoLibrary::common_value(std::string_view key, bool& mixed) const {
    std::string value;
    bool first = true;
    for (const auto& row : rows_) {
        if (!selected_.contains(row.value("id", Id{})))
            continue;
        const auto next = text_value(row, key);
        if (!first && next != value)
            mixed = true;
        if (first)
            value = next;
        first = false;
    }
    // Selected IDs outside this page have deliberately unknown values.
    const auto loaded = std::ranges::count_if(
        rows_, [this](const auto& row) { return selected_.contains(row.value("id", Id{})); });
    if (static_cast<std::size_t>(loaded) != selected_.size())
        mixed = true;
    return mixed ? std::string{} : value;
}

void PhotoLibrary::inspect() {
    if (selected_.empty())
        return;
    captured_ids_ = selected_ids();
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& [label, key] : std::array{
             std::pair{"Description", "description"}, std::pair{"Rating (0–5)", "rating"},
             std::pair{"Capture date override (empty restores extracted date)", "capture_date"}}) {
        bool mixed = false;
        auto value = common_value(key, mixed);
        fields.emplace_back(std::string{label} + (mixed ? " — mixed values" : ""), value);
    }
    fields.emplace_back("Add tags (comma separated; keeps existing tags)", "");
    fields.emplace_back("Remove tags (comma separated)", "");
    open_form(Form::inspect, std::move(fields));
}

void PhotoLibrary::edit_field(std::size_t index) {
    Json edit = {{"op", "edit"}, {"ids", captured_ids_}};
    if (index == 0)
        edit["description"] = fields_[0].text();
    else if (index == 1) {
        int rating = 0;
        if (!number(fields_[1].text(), rating) || rating < 0 || rating > 5) {
            form_message_ = "Enter a whole number from 0 to 5";
            return;
        }
        edit["rating"] = rating;
    } else if (index == 2)
        edit["capture_date"] = fields_[2].text().empty() ? Json{} : Json(fields_[2].text());
    else
        edit[index == 3 ? "add_tags" : "remove_tags"] = split(fields_[index].text());
    request(edit, [this](Json response) {
        if (response.value("ok", false)) {
            form_message_ = std::format("Saved for {} selected photos", captured_ids_.size());
            message_ = form_message_;
            query();
        }
    });
}

void PhotoLibrary::submit_form() {
    switch (form_) {
        case Form::source:
            if (!std::filesystem::path{fields_[0].text()}.is_absolute()) {
                form_message_ = "Enter an absolute folder path";
                break;
            }
            mutate({{"op", "add_source"},
                    {"path", fields_[0].text()},
                    {"recursive", recursive_},
                    {"exclusions", split(fields_[1].text())}},
                   "Folder added. Originals stay in place; Activity shows progress.");
            break;
        case Form::filters: {
            int rating = 0;
            if (!fields_[0].text().empty() &&
                (!number(fields_[0].text(), rating) || rating < 0 || rating > 5)) {
                form_message_ = "Minimum rating must be between 0 and 5";
                break;
            }
            filter_["rating_min"] = rating;
            filter_["date_from"] = fields_[1].text();
            filter_["date_to"] = fields_[2].text();
            offset_ = first_ = 0;
            close_form();
            query();
            break;
        }
        case Form::album:
            mutate({{"op", "create_album"}, {"name", fields_[0].text()}}, "Album created");
            break;
        case Form::save_filter: {
            Json filter = filter_;
            filter["text"] = search_.text();
            mutate({{"op", "save_filter"}, {"name", fields_[0].text()}, {"filter", filter}},
                   "Filter saved");
            break;
        }
        case Form::export_data: {
            const auto path = std::filesystem::path{fields_[0].text()};
            if (!path.is_absolute()) {
                form_message_ = "Enter an absolute filename";
                break;
            }
            request({{"op", "export"}}, [this, path](Json response) {
                if (!response.value("ok", false))
                    return;
                // The file is written off the event loop and existing files are never replaced.
                auto data = response.at("data").dump(2);
                const auto task = ++task_ids_;
                pending_.emplace(
                    task,
                    pool_.submit(
                        core::Priority::visible,
                        [path, data](const core::CancelToken&) {
                            const int fd =
                                ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                            if (fd < 0)
                                return std::string{"Cannot create export: "} + std::strerror(errno);
                            std::size_t written = 0;
                            bool failed = false;
                            while (written < data.size()) {
                                const auto count =
                                    ::write(fd, data.data() + written, data.size() - written);
                                if (count < 0 && errno == EINTR)
                                    continue;
                                if (count <= 0) {
                                    failed = true;
                                    break;
                                }
                                written += static_cast<std::size_t>(count);
                            }
                            if (::fsync(fd) != 0)
                                failed = true;
                            if (::close(fd) != 0)
                                failed = true;
                            return failed ? std::string{"Export failed; the incomplete file must be removed before retrying"}
                                      : std::string{};
                        },
                        [this, alive = alive_, task](std::string error) {
                            if (!*alive)
                                return;
                            pending_.erase(task);
                            if (error.empty()) {
                                close_form();
                                message_ = "Annotations exported";
                            } else
                                form_message_ = std::move(error);
                            redraw_();
                        }));
            });
            break;
        }
        case Form::restore_data: {
            const auto path = std::filesystem::path{fields_[0].text()};
            const auto task = ++task_ids_;
            pending_.emplace(
                task, pool_.submit(
                          core::Priority::visible,
                          [path](const core::CancelToken&) {
                              try {
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
                          [this, alive = alive_, task](Json result) {
                              if (!*alive)
                                  return;
                              pending_.erase(task);
                              if (!result.value("ok", false)) {
                                  form_message_ = "Cannot read this annotation export";
                                  redraw_();
                                  return;
                              }
                              mutate({{"op", "restore"}, {"data", result["data"]}},
                                     "Annotations restored");
                          }));
            break;
        }
        case Form::relink:
            if (!relink_reviewed_ || relink_path_ != fields_[0].text()) {
                form_message_ = "Review the match summary before relinking";
                break;
            }
            mutate({{"op", "relink_source"},
                    {"source_id", source_id_},
                    {"path", relink_path_},
                    {"confirm", true}},
                   "Source relinked. Existing organisation is retained.");
            break;
        case Form::forget:
            mutate({{"op", "forget_source"}, {"id", source_id_}},
                   "Source catalog data forgotten. Originals are unchanged.");
            break;
        case Form::duplicates: {
            Json ids = Json::array();
            for (const auto& photo : duplicate_group_.value("photos", Json::array())) {
                const auto id = photo.value("id", Id{});
                if (!keepers_.contains(id))
                    ids.push_back(id);
            }
            if (keepers_.empty() || ids.empty() || !file_action_) {
                form_message_ = "Keep at least one photo and choose at least one to trash";
                break;
            }
            Json kept = Json::array();
            for (const auto id : keepers_)
                kept.push_back(id);
            file_action_({{"kind", "trash"},
                          {"keeper_ids", kept},
                          {"ids", ids},
                          {"reviewed", duplicate_group_.value("photos", Json::array())}});
            close_form();
            break;
        }
        default:
            break;
    }
    redraw_();
}

void PhotoLibrary::activate(Action action) {
    if (!cursor_)
        return;
    const auto found = std::ranges::find_if(
        rows_, [this](const auto& row) { return row.value("id", Id{}) == cursor_; });
    if (found == rows_.end())
        return;
    if (found->value("availability", "") != "online" && action == Action::preview) {
        message_ = "The original is unavailable. Reconnect its source to open it.";
        redraw_();
        return;
    }
    action_(action, found->value("path", ""));
}

void PhotoLibrary::select(std::size_t index, ui::Modifiers modifiers) {
    if (index >= rows_.size())
        return;
    cursor_ = rows_[index].value("id", Id{});
    focus_ = -1;
    if (modifiers.shift) {
        auto from = std::ranges::find_if(
            rows_, [this](const auto& row) { return row.value("id", Id{}) == anchor_; });
        auto anchor = from == rows_.end() ? index : static_cast<std::size_t>(from - rows_.begin());
        if (!modifiers.ctrl)
            selected_.clear();
        for (auto i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
            selected_.insert(rows_[i].value("id", Id{}));
    } else if (modifiers.ctrl) {
        if (!selected_.erase(cursor_))
            selected_.insert(cursor_);
        anchor_ = cursor_;
    } else {
        selected_ = {cursor_};
        anchor_ = cursor_;
    }
    const auto visual_row = index / columns_;
    if (visual_row < first_)
        first_ = visual_row;
    else if (visual_row >= first_ + visible_)
        first_ = visual_row - visible_ + 1;
    redraw_();
}

void PhotoLibrary::move(int delta, ui::Modifiers modifiers) {
    if (rows_.empty())
        return;
    const auto found = std::ranges::find_if(
        rows_, [this](const auto& row) { return row.value("id", Id{}) == cursor_; });
    const auto index = found == rows_.end() ? 0 : found - rows_.begin();
    select(static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
               index + delta, 0, static_cast<std::ptrdiff_t>(rows_.size() - 1))),
           modifiers);
}

void PhotoLibrary::key(const ui::KeyEvent& event) {
    state_dirty_ = true;
    if (event.keysym == XKB_KEY_Escape) {
        if (form_ != Form::none)
            close_form();
        else if (!selected_.empty()) {
            selected_.clear();
            redraw_();
        } else
            action_(Action::close, {});
        return;
    }
    if (form_ != Form::none &&
        (event.keysym == XKB_KEY_Page_Down || event.keysym == XKB_KEY_Page_Up)) {
        const float delta = event.keysym == XKB_KEY_Page_Down ? form_content_.height * 0.8F
                                                              : -form_content_.height * 0.8F;
        form_scroll_ = std::clamp(form_scroll_ + delta, 0.0F,
                                  std::max(0.0F, form_content_height_ - form_content_.height));
        focus_ = -1;
        redraw_();
        return;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        ++edit_generation_;
        const auto count = field_count_ + static_cast<int>(controls_.size()) + 1;
        const int delta = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ? -1 : 1;
        focus_ = (focus_ + delta + count) % count;
        if (form_ != Form::none && editing()) {
            const auto bounds = fields_[static_cast<std::size_t>(focus_)].bounds();
            if (bounds.y < form_content_.y)
                form_scroll_ = std::max(0.0F, form_scroll_ - (form_content_.y - bounds.y) - 20);
            else if (bounds.bottom() > form_content_.bottom())
                form_scroll_ = std::min(std::max(0.0F, form_content_height_ - form_content_.height),
                                        form_scroll_ + bounds.bottom() - form_content_.bottom());
        }
        redraw_();
        return;
    }
    if (editing()) {
        if (event.keysym == XKB_KEY_Return) {
            if (form_ != Form::none && form_ != Form::inspect)
                submit_form();
            else if (form_ == Form::inspect)
                edit_field(static_cast<std::size_t>(focus_));
            else
                focus_ = -1;
        } else {
            auto& active =
                form_ == Form::none ? search_ : fields_[static_cast<std::size_t>(focus_)];
            const auto old = active.text();
            active.key(event);
            if (form_ == Form::none && old != active.text())
                search_changed();
        }
        redraw_();
        return;
    }
    if (focus_ >= field_count_ && focus_ < field_count_ + static_cast<int>(controls_.size())) {
        if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_space) {
            auto control = controls_[static_cast<std::size_t>(focus_ - field_count_)];
            if (control.enabled)
                control.run();
        }
        return;
    }
    if (form_ != Form::none)
        return;
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_f || event.keysym == XKB_KEY_F)) {
        focus_ = 0;
        redraw_();
        return;
    }
    if (destination_ != Destination::library) {
        if (event.keysym == XKB_KEY_F5) {
            refresh();
            schedule_work();
        }
        return;
    }
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A)) {
        for (const auto& row : rows_)
            selected_.insert(row.value("id", Id{}));
        redraw_();
        return;
    }
    switch (event.keysym) {
        case XKB_KEY_Up:
            move(-static_cast<int>(columns_), event.modifiers);
            break;
        case XKB_KEY_Down:
            move(static_cast<int>(columns_), event.modifiers);
            break;
        case XKB_KEY_Left:
            move(-1, event.modifiers);
            break;
        case XKB_KEY_Right:
            move(1, event.modifiers);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_space:
            activate(Action::preview);
            break;
        case XKB_KEY_F5:
            refresh();
            schedule_work();
            break;
        default:
            break;
    }
}

void PhotoLibrary::paste(std::string_view text) {
    state_dirty_ = true;
    if (!editing())
        return;
    auto& active = form_ == Form::none ? search_ : fields_[static_cast<std::size_t>(focus_)];
    active.insert(text);
    if (form_ == Form::none)
        search_changed();
    redraw_();
}

std::string PhotoLibrary::copy_selection(bool cut) {
    state_dirty_ = true;
    if (!editing())
        return {};
    auto& active = form_ == Form::none ? search_ : fields_[static_cast<std::size_t>(focus_)];
    auto result = cut ? active.cut() : active.selected_text();
    if (cut && form_ == Form::none && !result.empty())
        search_changed();
    redraw_();
    return result;
}

void PhotoLibrary::pointer(const ui::PointerEvent& event) {
    if (event.kind == ui::PointerEvent::Kind::press || event.kind == ui::PointerEvent::Kind::scroll)
        state_dirty_ = true;
    if (editing()) {
        auto& active = form_ == Form::none ? search_ : fields_[static_cast<std::size_t>(focus_)];
        if (event.kind != ui::PointerEvent::Kind::press && active.pointer(event)) {
            redraw_();
            return;
        }
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && form_ != Form::none &&
        form_content_.contains(event.x, event.y)) {
        const float delta = event.wheel_y != 0 ? event.wheel_y * 48 : event.scroll_y;
        form_scroll_ = std::clamp(form_scroll_ + delta, 0.0F,
                                  std::max(0.0F, form_content_height_ - form_content_.height));
        focus_ = -1;
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && list_.contains(event.x, event.y) &&
        form_ == Form::none) {
        const int delta =
            static_cast<int>(event.wheel_y != 0 ? event.wheel_y * 3 : event.scroll_y / 32);
        const auto count = destination_ == Destination::library
                               ? (rows_.size() + columns_ - 1) / columns_
                               : records_.size();
        auto& first = destination_ == Destination::library ? first_ : records_first_;
        first = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(first) + delta, 0,
            static_cast<std::ptrdiff_t>(count > visible_ ? count - visible_ : 0)));
        redraw_();
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    for (int i = 0; i < field_count_; ++i) {
        auto& target = form_ == Form::none ? search_ : fields_[static_cast<std::size_t>(i)];
        if (target.bounds().contains(event.x, event.y) &&
            (form_ == Form::none || form_content_.contains(event.x, event.y))) {
            focus_ = i;
            ++edit_generation_;
            target.pointer(event);
            redraw_();
            return;
        }
    }
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const auto control = controls_[i];
        if (control.bounds.contains(event.x, event.y)) {
            focus_ = field_count_ + static_cast<int>(i);
            if (control.enabled)
                control.run();
            redraw_();
            return;
        }
    }
    if (form_ != Form::none)
        return;
    for (const auto& hit : row_hits_) {
        if (hit.bounds.contains(event.x, event.y)) {
            select(hit.index, event.modifiers);
            if (event.click_count == 2)
                activate(Action::preview);
            return;
        }
    }
}

ui::CursorShape PhotoLibrary::cursor_at(float x, float y) const {
    if (form_ == Form::none && search_.bounds().contains(x, y))
        return ui::CursorShape::text;
    if (form_ != Form::none && form_content_.contains(x, y))
        for (int i = 0; i < field_count_; ++i)
            if (fields_[static_cast<std::size_t>(i)].bounds().contains(x, y))
                return ui::CursorShape::text;
    for (const auto& control : controls_)
        if (control.enabled && control.bounds.contains(x, y))
            return ui::CursorShape::pointer;
    return ui::CursorShape::standard;
}

void PhotoLibrary::button(ui::Renderer& renderer, const theme::Palette& palette, ui::Rect bounds,
                          std::string label, std::function<void()> run, bool enabled) {
    const auto index = field_count_ + static_cast<int>(controls_.size());
    renderer.draw_box(bounds, {.fill = palette.surface_container_highest,
                               .radius = {},
                               .border_width = focus_ == index ? 2.0F : 0.0F,
                               .border = palette.primary});
    renderer.draw_text(label, bounds.x + 8, bounds.y + 7, 12,
                       enabled ? palette.on_surface : palette.on_surface_variant.with_alpha(0.45F),
                       bounds.width - 16);
    controls_.push_back({std::move(label), bounds, std::move(run), enabled});
}

void PhotoLibrary::field(ui::Renderer& renderer, const theme::Palette& palette, std::size_t index,
                         ui::Rect bounds) {
    auto& value = form_ == Form::none ? search_ : fields_[index];
    value.draw(
        renderer, bounds, 14,
        {.background = palette.surface_container,
         .border = focus_ == static_cast<int>(index) ? palette.primary : palette.outline_variant,
         .text = palette.on_surface,
         .selection = palette.primary_container,
         .caret =
             focus_ == static_cast<int>(index) ? palette.primary : palette.primary.with_alpha(0)});
}

void PhotoLibrary::draw_thumbnail(ui::Renderer& renderer, const Json& row, ui::Rect bounds,
                                  const theme::Palette& palette) {
    const ui::Texture* texture = nullptr;
    if (thumbnails_)
        texture = thumbnails_->get(row.value("path", ""), row.value("mtime_ns", std::int64_t{}),
                                   thumbs::Size::large, {}, nullptr,
                                   row.value("availability", "") != "online");
    if (texture) {
        const float scale = std::min(bounds.width / static_cast<float>(texture->width()),
                                     bounds.height / static_cast<float>(texture->height()));
        const float width = static_cast<float>(texture->width()) * scale;
        const float height = static_cast<float>(texture->height()) * scale;
        renderer.draw_image(*texture, {bounds.x + (bounds.width - width) / 2,
                                       bounds.y + (bounds.height - height) / 2, width, height});
    } else {
        renderer.fill_rect(bounds, palette.surface_container_highest);
        renderer.draw_text(row.value("availability", "") == "online"
                               ? "Photo"
                               : row.value("availability", "Unavailable"),
                           bounds.x + 8, bounds.y + bounds.height / 2 - 6, 12,
                           palette.on_surface_variant, bounds.width - 16);
    }
}

void PhotoLibrary::draw_photos(ui::Renderer& renderer, ui::Rect bounds,
                               const theme::Palette& palette) {
    const auto columns =
        grid_ ? std::max<std::size_t>(1, static_cast<std::size_t>(bounds.width / 184)) : 1;
    if (!rows_.empty()) {
        if (scroll_anchor_) {
            first_ = *scroll_anchor_ / columns;
            scroll_anchor_.reset();
            state_dirty_ = true;
        } else if (columns != columns_) {
            first_ = first_ * columns_ / columns;
            state_dirty_ = true;
        }
        first_ = std::min(first_, (rows_.size() - 1) / columns);
    }
    columns_ = columns;
    const float row_height = grid_ ? 190.0F : 74.0F;
    visible_ =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(bounds.height / row_height)));
    const float cell_width = bounds.width / static_cast<float>(columns_);
    renderer.push_clip(bounds);
    for (std::size_t index = first_ * columns_;
         index < rows_.size() && index < (first_ + visible_) * columns_; ++index) {
        const auto& row = rows_[index];
        const auto id = row.value("id", Id{});
        const ui::Rect cell{bounds.x + static_cast<float>(index % columns_) * cell_width,
                            bounds.y + static_cast<float>(index / columns_ - first_) * row_height,
                            cell_width - 6, row_height - 6};
        const bool chosen = selected_.contains(id);
        renderer.draw_box(cell,
                          {.fill = chosen ? palette.primary_container : palette.surface_container,
                           .radius = {},
                           .border_width = id == cursor_ ? 1.0F : 0.0F,
                           .border = palette.primary});
        const ui::Rect picture{cell.x + 6, cell.y + 6, grid_ ? cell.width - 12 : 66,
                               grid_ ? 116.0F : 54.0F};
        draw_thumbnail(renderer, row, picture, palette);
        const float x = grid_ ? cell.x + 8 : cell.x + 82;
        const float y = grid_ ? cell.y + 128 : cell.y + 7;
        const float width = grid_ ? cell.width - 16 : cell.width - 90;
        const auto ink = chosen ? palette.on_primary_container : palette.on_surface;
        const auto secondary =
            chosen ? palette.on_primary_container.with_alpha(0.8F) : palette.on_surface_variant;
        renderer.draw_text(row.value("filename", ""), x, y, 13, ink, width);
        renderer.draw_text(std::filesystem::path{row.value("path", "")}.parent_path().string(), x,
                           y + 19, 11, secondary, width);
        renderer.draw_text(
            std::format("{} · Rating {}{}",
                        text_value(row, "capture_date").empty() ? "Unknown date"
                                                                : text_value(row, "capture_date"),
                        row.value("rating", 0),
                        row.value("availability", "") == "online"
                            ? ""
                            : " · " + row.value("availability", "")),
            x, y + 36, 11, secondary, width);
        row_hits_.push_back({cell, index});
    }
    if (rows_.empty())
        renderer.draw_text(
            querying_ ? "Finding photos…" : "No matching photos. Add a folder or clear filters.",
            bounds.x + 12, bounds.y + 28, 14, palette.on_surface_variant, bounds.width - 24);
    renderer.pop_clip();
}

void PhotoLibrary::draw_records(ui::Renderer& renderer, ui::Rect bounds,
                                const theme::Palette& palette) {
    visible_ = std::max<std::size_t>(1, static_cast<std::size_t>(bounds.height / 112));
    renderer.push_clip(bounds);
    for (std::size_t i = records_first_; i < records_.size() && i < records_first_ + visible_;
         ++i) {
        const auto record = records_[i];
        const auto id = record.value("id", Id{});
        const float y = bounds.y + static_cast<float>(i - records_first_) * 112;
        renderer.fill_rect({bounds.x, y, bounds.width, 106}, palette.surface_container);
        std::string title, detail;
        if (destination_ == Destination::activity && record.value("file_operation", false)) {
            title =
                "File operation: " + record.value("kind", "") + " · " + record.value("state", "");
            detail = std::format("{} photos · {}", record.value("photos", Json::array()).size(),
                                 record.value("error", ""));
            button(renderer, palette, {bounds.x + 8, y + 66, 150, 30}, "Review outcome",
                   [this, record] {
                       duplicate_group_ = record;
                       open_form(Form::operation, {});
                   });
            if (record.value("state", "") == "pending")
                button(renderer, palette, {bounds.x + 166, y + 66, 180, 30}, "Recheck receipts",
                       [this] {
                           request({{"op", "recover_operations"}}, [this](Json) { refresh(); });
                       });
        } else if (destination_ == Destination::activity) {
            title = record.value("kind", "Job") + " · " + record.value("state", "");
            detail = std::format("{} completed", record.value("completed", 0));
            if (record.contains("total") && !record["total"].is_null())
                detail += " / " + record["total"].dump();
            if (!record.value("error", "").empty())
                detail += " · " + record.value("error", "");
            float x = bounds.x + 8;
            const auto state = record.value("state", "");
            for (const auto& [label, action] :
                 std::array{std::pair{"Pause", "pause"}, std::pair{"Resume", "resume"},
                            std::pair{"Cancel", "cancel"}, std::pair{"Retry", "retry"}}) {
                const bool enabled = action == std::string_view{"pause"}
                                         ? state == "running" || state == "queued"
                                     : action == std::string_view{"resume"} ? state == "paused"
                                     : action == std::string_view{"retry"}
                                         ? state == "failed" || state == "waiting"
                                         : state != "completed" && state != "cancelled";
                button(
                    renderer, palette, {x, y + 66, 78, 30}, label,
                    [this, id, action] {
                        mutate({{"op", "job_control"}, {"id", id}, {"action", action}},
                               "Job updated at the next safe checkpoint");
                    },
                    enabled);
                x += 84;
            }
        } else if (destination_ == Destination::sources) {
            title = record.value("path", "");
            detail =
                record.value("availability", "") +
                (record.value("active", true) ? " · Active" : " · Inactive (annotations retained)");
            button(
                renderer, palette, {bounds.x + 8, y + 66, 100, 30}, "Refresh folder",
                [this, id] { mutate({{"op", "refresh"}, {"source_id", id}}, "Refresh queued"); });
            button(renderer, palette, {bounds.x + 116, y + 66, 100, 30}, "Show photos", [this, id] {
                filter_["source_id"] = id;
                offset_ = first_ = 0;
                switch_to(Destination::library);
            });
            button(
                renderer, palette, {bounds.x + 224, y + 66, 142, 30}, "Make inactive",
                [this, id] {
                    mutate({{"op", "remove_source"}, {"id", id}},
                           "Source inactive. Originals and annotations are retained.");
                },
                record.value("active", true));
            button(renderer, palette, {bounds.x + 374, y + 66, 100, 30}, "Relink source",
                   [this, id, record] {
                       source_id_ = id;
                       relink_reviewed_ = false;
                       open_form(Form::relink, {{"New folder location", record.value("path", "")}});
                   });
            button(
                renderer, palette, {bounds.x + 482, y + 66, 145, 30}, "Forget source data…",
                [this, id, record] {
                    source_id_ = id;
                    open_form(Form::forget, {});
                    form_message_ = record.value("path", "");
                },
                !record.value("active", true));
        } else if (destination_ == Destination::albums) {
            title = record.value("name", "");
            detail = std::format("{} photos", record.value("count", 0));
            button(renderer, palette, {bounds.x + 8, y + 66, 100, 30}, "Show album", [this, id] {
                filter_["album_id"] = id;
                offset_ = first_ = 0;
                switch_to(Destination::library);
            });
            button(
                renderer, palette, {bounds.x + 116, y + 66, 140, 30}, "Add selected photos",
                [this, id, ids = selected_ids()] {
                    mutate({{"op", "edit"}, {"ids", ids}, {"add_albums", {id}}},
                           "Album membership saved");
                },
                !selected_.empty());
            button(
                renderer, palette, {bounds.x + 264, y + 66, 160, 30}, "Remove selected photos",
                [this, id, ids = selected_ids()] {
                    mutate({{"op", "edit"}, {"ids", ids}, {"remove_albums", {id}}},
                           "Album membership saved");
                },
                !selected_.empty());
        } else if (destination_ == Destination::filters) {
            title = record.value("name", "");
            detail = record.value("filter", Json::object()).dump();
            button(renderer, palette, {bounds.x + 8, y + 66, 120, 30}, "Find photos",
                   [this, record] {
                       filter_ = record.value("filter", Json::object());
                       search_.set_text(filter_.value("text", ""));
                       offset_ = first_ = 0;
                       switch_to(Destination::library);
                   });
        } else {
            title = std::format("{} duplicate candidates",
                                record.value("photos", Json::array()).size());
            detail = record.value("reason", "Choose which originals to keep");
            button(renderer, palette, {bounds.x + 8, y + 66, 158, 30}, "Review these copies",
                   [this, record] {
                       duplicate_group_ = record;
                       keepers_ = {record.value("keeper_id", Id{})};
                       open_form(Form::duplicates, {});
                   });
        }
        renderer.draw_text(title, bounds.x + 10, y + 8, 14, palette.on_surface, bounds.width - 20);
        renderer.draw_text(detail, bounds.x + 10, y + 31, 12, palette.on_surface_variant,
                           bounds.width - 20);
    }
    if (records_.empty())
        renderer.draw_text(
            destination_ == Destination::review ? "No exact duplicate groups. Choose Find exact "
                                                  "duplicates to compare indexed photos."
                                                : "Nothing here yet.",
            bounds.x + 12, bounds.y + 24, 14, palette.on_surface_variant, bounds.width - 24);
    renderer.pop_clip();
}

const std::vector<std::string>& PhotoLibrary::wrapped_path(ui::Renderer& renderer,
                                                           const std::string& text, float width) {
    auto& cached = wrapped_paths_[text];
    if (cached.first == width && !cached.second.empty())
        return cached.second;
    cached.first = width;
    cached.second.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        std::vector<std::size_t> boundaries{start};
        for (auto i = start + 1; i <= text.size(); ++i)
            if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U)
                boundaries.push_back(i);
        std::size_t low = 1, high = boundaries.size() - 1, fitting = 1;
        while (low <= high) {
            const auto middle = low + (high - low) / 2;
            const auto part = std::string_view{text}.substr(start, boundaries[middle] - start);
            const auto measured =
                renderer.fonts().layout(part, 12, renderer.scale()).width / renderer.scale();
            if (measured <= width) {
                fitting = middle;
                low = middle + 1;
            } else {
                if (middle == 0)
                    break;
                high = middle - 1;
            }
        }
        auto end = boundaries[fitting];
        if (end < text.size()) {
            const auto slash = text.rfind('/', end - 1);
            if (slash != std::string::npos && slash > start + (end - start) / 2)
                end = slash + 1;
        }
        cached.second.push_back(text.substr(start, end - start));
        start = end;
    }
    if (cached.second.empty())
        cached.second.emplace_back();
    return cached.second;
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

void PhotoLibrary::draw_form(ui::Renderer& renderer, ui::Rect bounds,
                             const theme::Palette& palette) {
    controls_.clear();
    row_hits_.clear();
    renderer.fill_rect(bounds, palette.surface);
    const float width = std::max(1.0F, std::min(720.0F, bounds.width - 32));
    const float left = bounds.x + (bounds.width - width) / 2;
    const float top = bounds.y + 16;
    const float footer = bounds.bottom() - 84;
    std::string title;
    switch (form_) {
        case Form::source:
            title = "Add folder to Photo Library";
            break;
        case Form::inspect:
            title = std::format("Edit {} selected photos", captured_ids_.size());
            break;
        case Form::filters:
            title = "Find photos";
            break;
        case Form::album:
            title = "Create album";
            break;
        case Form::save_filter:
            title = "Save current filter";
            break;
        case Form::export_data:
            title = "Export annotations";
            break;
        case Form::restore_data:
            title = "Restore annotations";
            break;
        case Form::duplicates:
            title = "Review duplicate candidates";
            break;
        case Form::relink:
            title = "Relink source";
            break;
        case Form::forget:
            title = "Forget this source’s catalog data?";
            break;
        case Form::operation:
            title = "File operation outcome";
            break;
        default:
            break;
    }
    renderer.draw_text(title, left, top, 20, palette.on_surface, width);
    const auto explanation =
        form_ == Form::forget
            ? "Annotations and album memberships will be removed. Originals stay unchanged."
        : form_ == Form::duplicates ? "Scroll to inspect every full location. Keep at least one "
                                      "copy. Originals are rechecked before trash."
        : form_ == Form::inspect
            ? "Catalog overrides only. Apply fields separately; other values stay unchanged."
        : form_ == Form::source
            ? "Originals stay in place. Indexing starts only after you add this folder."
        : form_ == Form::relink
            ? "Review matched photos before reconnecting. Ambiguous matches stay unresolved."
            : "Changes are stored locally in your photo catalog.";
    renderer.draw_text(explanation, left, top + 30, 12, palette.on_surface_variant, width);
    form_content_ = {left, top + 60, width, std::max(1.0F, footer - top - 72)};
    form_scroll_ =
        std::clamp(form_scroll_, 0.0F, std::max(0.0F, form_content_height_ - form_content_.height));
    const float start = form_content_.y - form_scroll_;
    float y = start;
    const auto visible = [&](float row_y, float height) {
        return row_y + height > form_content_.y && row_y < form_content_.bottom();
    };
    const auto body_button = [&](ui::Rect rect, std::string label, std::function<void()> run,
                                 bool enabled = true) {
        if (visible(rect.y, rect.height)) {
            button(renderer, palette, rect, std::move(label), std::move(run), enabled);
            if (enabled && !controls_.empty()) {
                auto& hit = controls_.back().bounds;
                const float bottom = std::min(hit.bottom(), form_content_.bottom());
                hit.y = std::max(hit.y, form_content_.y);
                hit.height = std::max(0.0F, bottom - hit.y);
            }
        }
    };
    const auto path_text = [&](const std::string& path, float x, float row_y, float available) {
        const auto& lines = wrapped_path(renderer, path, available);
        for (const auto& line : lines) {
            renderer.draw_text(line, x, row_y, 12, palette.on_surface, available);
            row_y += 18;
        }
        return static_cast<float>(lines.size()) * 18;
    };
    renderer.push_clip(form_content_);
    if (form_ == Form::duplicates) {
        renderer.draw_text(duplicate_group_.value("reason", ""), left, y, 12,
                           palette.on_surface_variant, width);
        y += 32;
        for (const auto& photo : duplicate_group_.value("photos", Json::array())) {
            const auto id = photo.value("id", Id{});
            const auto path = photo.value("path", "");
            const float available = std::max(80.0F, width - 174);
            const auto& lines = wrapped_path(renderer, path, available);
            const float height = std::max(84.0F, static_cast<float>(lines.size()) * 18 + 38);
            if (visible(y, height)) {
                renderer.fill_rect({left, y, width, height - 6}, palette.surface_container);
                body_button({left + 6, y + 12, 84, 32}, keepers_.contains(id) ? "✓ Keep" : "Trash",
                            [this, id] {
                                if (!keepers_.erase(id))
                                    keepers_.insert(id);
                            });
                draw_thumbnail(renderer, photo, {left + 98, y + 8, 58, 58}, palette);
                const auto used = path_text(path, left + 168, y + 8, available);
                renderer.draw_text(std::format("{} bytes · {} · {}", photo.value("size", 0),
                                               text_value(photo, "capture_date").empty()
                                                   ? "Unknown date"
                                                   : text_value(photo, "capture_date"),
                                               photo.value("availability", "")),
                                   left + 168, y + used + 12, 11, palette.on_surface_variant,
                                   available);
            }
            y += height;
        }
    } else if (form_ == Form::operation) {
        renderer.draw_text("Status: " + duplicate_group_.value("state", "") + " · " +
                               duplicate_group_.value("error", ""),
                           left, y, 12, palette.on_surface_variant, width);
        y += 30;
        for (const auto& photo : duplicate_group_.value("photos", Json::array())) {
            y += path_text(photo.value("path", ""), left, y, width);
            const auto results = duplicate_group_.value("results", Json::array());
            const auto found = std::ranges::find_if(results, [&](const auto& result) {
                return result.value("id", Id{}) == photo.value("id", Id{});
            });
            const auto outcome =
                found == results.end()
                    ? std::string{"Outcome not confirmed; nothing will be repeated automatically"}
                : found->value("success", false) ? "Completed: " + found->value("destination", "")
                                                 : "Failed: " + found->value("error", "");
            y += path_text(outcome, left, y + 4, width) + 20;
        }
    } else {
        if (form_ == Form::inspect && captured_ids_.size() == 1) {
            const auto found = std::ranges::find_if(rows_, [this](const auto& row) {
                return row.value("id", Id{}) == captured_ids_.front().get<Id>();
            });
            if (found != rows_.end()) {
                draw_thumbnail(renderer, *found, {left, y, 96, 80}, palette);
                float used =
                    path_text(found->value("path", ""), left + 108, y, std::max(1.0F, width - 108));
                for (const auto& line :
                     {"Availability: " + found->value("availability", "") +
                          " · Date origin: " + found->value("date_origin", ""),
                      "Tags: " + text_value(*found, "tags"),
                      "Albums: " + text_value(*found, "albums"),
                      std::format("{} × {} · {}", found->value("width", 0),
                                  found->value("height", 0), found->value("camera", ""))}) {
                    renderer.draw_text(line, left + 108, y + used, 12, palette.on_surface_variant,
                                       width - 108);
                    used += 19;
                }
                y += std::max(96.0F, used + 16);
            }
        }
        for (int i = 0; i < field_count_; ++i) {
            const auto index = static_cast<std::size_t>(i);
            renderer.draw_text(labels_[index], left, y, 12, palette.on_surface_variant, width);
            field(renderer, palette, index,
                  {left, y + 20, width - (form_ == Form::inspect ? 88 : 0), 32});
            if (form_ == Form::inspect)
                body_button({left + width - 80, y + 20, 80, 32}, "Apply",
                            [this, index] { edit_field(index); });
            y += 66;
        }
        if (form_ == Form::source) {
            body_button({left, y, width, 30},
                        recursive_ ? "✓ Include subfolders" : "Include subfolders",
                        [this] { recursive_ = !recursive_; });
            y += 42;
        }
        if (form_ == Form::filters) {
            body_button(
                {left, y, 190, 30},
                filter_.value("unknown_date", false) ? "✓ Unknown date only" : "Unknown date only",
                [this] { filter_["unknown_date"] = !filter_.value("unknown_date", false); });
            y += 38;
            body_button({left, y, 220, 30},
                        filter_.value("include_inactive", false) ? "✓ Include inactive sources"
                                                                 : "Include inactive sources",
                        [this] {
                            filter_["include_inactive"] = !filter_.value("include_inactive", false);
                        });
            y += 42;
        }
        if (form_ == Form::relink) {
            body_button({left, y, 170, 32}, "Review matches", [this] {
                relink_reviewed_ = false;
                const auto path = fields_[0].text();
                const auto generation = edit_generation_;
                request({{"op", "relink_source"},
                         {"source_id", source_id_},
                         {"path", path},
                         {"confirm", false}},
                        [this, path, generation](Json response) {
                            if (generation != edit_generation_ || form_ != Form::relink ||
                                !response.value("ok", false))
                                return;
                            const auto matched = response.value("matched", Json::array()).size();
                            const auto missing = response.value("missing", Json::array()).size();
                            const auto ambiguous =
                                response.value("ambiguous", Json::array()).size();
                            form_message_ = std::format("Matched: {} · Missing: {} · Ambiguous: {}",
                                                        matched, missing, ambiguous);
                            relink_reviewed_ = matched > 0 && ambiguous == 0;
                            relink_path_ = path;
                        });
            });
            y += 42;
        }
    }
    form_content_height_ = std::max(0.0F, y - start);
    renderer.pop_clip();
    if (form_content_height_ > form_content_.height) {
        const float thumb_height =
            std::max(16.0F, form_content_.height * form_content_.height / form_content_height_);
        const float range = std::max(1.0F, form_content_height_ - form_content_.height);
        const float thumb_y =
            form_content_.y + (form_content_.height - thumb_height) * form_scroll_ / range;
        renderer.fill_rect({left + width + 6, thumb_y, 3, thumb_height}, palette.outline_variant);
    }
    button(renderer, palette, {left, footer, 110, 32},
           form_ == Form::inspect || form_ == Form::operation ? "Done" : "Cancel",
           [this] { close_form(); });
    if (form_ == Form::duplicates) {
        const auto count = duplicate_group_.value("photos", Json::array()).size();
        button(
            renderer, palette, {left + 122, footer, 240, 32},
            std::format("Keep {} · Trash {}", keepers_.size(), count - keepers_.size()),
            [this] { submit_form(); }, !keepers_.empty() && keepers_.size() < count);
    } else if (form_ == Form::inspect && file_action_) {
        const auto restorable = std::ranges::count_if(rows_, [this](const auto& row) {
            return std::ranges::any_of(captured_ids_,
                                       [&](const auto& id) {
                                           return id.template get<Id>() == row.value("id", Id{});
                                       }) &&
                   row.value("availability", "") == "trashed";
        });
        button(
            renderer, palette, {left + 122, footer, 205, 32}, "Restore selected from trash",
            [this, ids = captured_ids_] { file_action_({{"kind", "restore"}, {"ids", ids}}); },
            static_cast<std::size_t>(restorable) == captured_ids_.size());
    } else if (form_ != Form::inspect && form_ != Form::operation)
        button(
            renderer, palette, {left + 122, footer, 140, 32},
            form_ == Form::source   ? "Add folder"
            : form_ == Form::forget ? "Forget data"
                                    : "Apply",
            [this] { submit_form(); },
            form_ != Form::relink || (relink_reviewed_ && relink_path_ == fields_[0].text()));
    renderer.draw_text(form_message_, left, footer + 44, 12, palette.on_surface_variant, width);
}

void PhotoLibrary::draw(ui::Renderer& renderer, ui::Rect bounds, const theme::Palette& palette) {
    if (!open_)
        return;
    renderer.fill_rect(bounds, palette.surface);
    renderer.push_clip(bounds);
    controls_.clear();
    row_hits_.clear();
    const float left = bounds.x + 16, width = std::max(1.0F, bounds.width - 32);
    renderer.draw_text("Photo Library", left, bounds.y + 12, 20, palette.on_surface, width);
    float x = left, y = bounds.y + 45;
    for (const auto& [label, destination] : std::array{
             std::pair{"Library", Destination::library}, std::pair{"Review", Destination::review},
             std::pair{"Activity", Destination::activity},
             std::pair{"Sources", Destination::sources}, std::pair{"Albums", Destination::albums},
             std::pair{"Saved filters", Destination::filters}}) {
        if (x + 96 > left + width && x > left) {
            x = left;
            y += 36;
        }
        button(renderer, palette, {x, y, 96, 30},
               std::string{destination_ == destination ? "• " : ""} + label,
               [this, destination] { switch_to(destination); });
        x += 102;
    }
    button(renderer, palette, {bounds.right() - 100, bounds.y + 8, 84, 30}, "Close",
           [this] { action_(Action::close, {}); });
    y += 42;
    if (form_ == Form::none)
        field_count_ = 1;
    renderer.draw_text("Find by filename, description or tags", left, y, 12,
                       palette.on_surface_variant, width);
    search_.draw(
        renderer, {left, y + 19, width, 34}, 14,
        {.background = palette.surface_container,
         .border = form_ == Form::none && focus_ == 0 ? palette.primary : palette.outline_variant,
         .text = palette.on_surface,
         .selection = palette.primary_container,
         .caret =
             form_ == Form::none && focus_ == 0 ? palette.primary : palette.primary.with_alpha(0)});
    y += 64;
    x = left;
    const auto tool = [&](std::string label, std::function<void()> run, bool enabled = true) {
        const float size = std::min(
            width,
            renderer.fonts().layout(label, 12, renderer.scale()).width / renderer.scale() + 20);
        if (x + size > left + width && x > left) {
            x = left;
            y += 36;
        }
        button(renderer, palette, {x, y, size, 30}, std::move(label), std::move(run), enabled);
        x += size + 6;
    };
    tool("Add folder", [this] { add_folder(); });
    if (destination_ == Destination::library) {
        tool(grid_ ? "Details view" : "Gallery view", [this] {
            scroll_anchor_ = first_ * columns_;
            grid_ = !grid_;
        });
        tool("Filters", [this] {
            open_form(Form::filters,
                      {{"Minimum rating (0–5)", std::to_string(filter_.value("rating_min", 0))},
                       {"Capture date from (YYYY-MM-DD)", filter_.value("date_from", "")},
                       {"Capture date to (YYYY-MM-DD)", filter_.value("date_to", "")}});
        });
        tool("Clear filters", [this] {
            filter_ = Json::object();
            search_.set_text("");
            offset_ = first_ = 0;
            query();
        });
        tool("Sort", [this] {
            const auto sort = filter_.value("sort", "name");
            filter_["sort"] = sort == "name" ? "date" : sort == "date" ? "rating" : "name";
            query();
        });
        tool("Edit selected", [this] { inspect(); }, !selected_.empty());
        tool("Quick Look", [this] { activate(Action::preview); }, cursor_ != 0);
        tool("Show in folder", [this] { activate(Action::reveal); }, cursor_ != 0);
        tool("Select loaded photos", [this] {
            for (const auto& row : rows_)
                selected_.insert(row.value("id", Id{}));
        });
        tool("Clear selection", [this] { selected_.clear(); });
        tool("Save filter", [this] { open_form(Form::save_filter, {{"Filter name", ""}}); });
    } else if (destination_ == Destination::review) {
        tool("Find exact duplicates",
             [this] { mutate({{"op", "find_duplicates"}}, "Exact duplicate review queued"); });
    } else if (destination_ == Destination::albums)
        tool("Create album", [this] { open_form(Form::album, {{"Album name", ""}}); });
    tool("Refresh", [this] {
        refresh();
        schedule_work();
    });
    tool(
        background_paused_ ? "Resume background work" : "Pause background work",
        [this] { control_background({{"paused", !background_paused_}}); }, background_ready_);
    tool(
        background_pace_ == "gentle" ? "Pace: Gentle" : "Pace: Normal",
        [this] {
            control_background({{"pace", background_pace_ == "normal" ? "gentle" : "normal"}});
        },
        background_ready_);
    if (destination_ == Destination::sources) {
        tool("Refresh library", [this] { mutate({{"op", "refresh"}}, "Library refresh queued"); });
        tool("Export annotations",
             [this] { open_form(Form::export_data, {{"New export filename", ""}}); });
        tool("Restore annotations",
             [this] { open_form(Form::restore_data, {{"Export filename to restore", ""}}); });
    }
    y += 40;
    if (destination_ == Destination::library) {
        renderer.draw_text(
            std::format("{} matches · {} selected photos · {} loaded · Sort: {}{}", total_,
                        selected_.size(), rows_.size(), filter_.value("sort", "name"),
                        querying_ ? " · Updating…" : ""),
            left, y, 12, palette.on_surface_variant, width);
        y += 25;
    }
    list_ = {left, y, width, std::max(0.0F, bounds.bottom() - y - 106)};
    if (destination_ == Destination::library)
        draw_photos(renderer, list_, palette);
    else
        draw_records(renderer, list_, palette);
    const float footer = bounds.bottom() - 94;
    if (destination_ == Destination::library) {
        button(
            renderer, palette, {left, footer, 110, 30}, "Previous page",
            [this] {
                offset_ = offset_ >= page_size_ ? offset_ - page_size_ : 0;
                first_ = 0;
                query();
            },
            offset_ > 0);
        button(
            renderer, palette, {left + 118, footer, 98, 30}, "Next page",
            [this] {
                offset_ += page_size_;
                first_ = 0;
                query();
            },
            offset_ + rows_.size() < total_);
        renderer.draw_text(
            std::format("Page {} · Ctrl+A selects this loaded page only", offset_ / page_size_ + 1),
            left + 230, footer + 8, 12, palette.on_surface_variant, width - 230);
    }
    renderer.draw_text(background_paused_ ? "Background paused · " + progress_ : progress_, left,
                       bounds.bottom() - 50, 12, palette.on_surface_variant, width);
    renderer.draw_text(message_, left, bounds.bottom() - 27, 12, palette.on_surface_variant, width);
    if (form_ != Form::none)
        draw_form(renderer, bounds, palette);
    renderer.pop_clip();
}
}  // namespace rummage::views
