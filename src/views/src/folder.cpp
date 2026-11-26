#include "rummage/views/folder.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "rummage/fs/name_pattern.hpp"
#include "rummage/picture/load.hpp"

namespace rummage::views {
namespace {

// Sniffing batches stay small so a fast scroll does not queue minutes of reads.
constexpr std::size_t kMaxSniffBatch = 64;

bool hidden_name(const std::string& name) { return !name.empty() && name.front() == '.'; }

}  // namespace

Folder::Folder(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
               std::shared_ptr<const fs::MimeDatabase> mime, std::filesystem::path directory,
               std::function<void()> redraw, bool show_hidden)
    : pool_{pool},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      model_{loop, pool, watcher, mime_} {
    model_.set_change_handler([this](const fs::ModelChange& change) { on_change(change); });
    model_.set_filter(show_hidden, {});
    model_.open(std::move(directory));
}

Folder::Folder(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
               std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw)
    : pool_{pool},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      model_{loop, pool, watcher, mime_},
      collection_{true} {}

void Folder::replace_collection(std::vector<fs::Entry> rows, std::string label, std::size_t total,
                                bool loading) {
    if (!collection_)
        return;
    collection_entries_ = std::move(rows);
    collection_label_ = std::move(label);
    collection_total_ = total;
    collection_loading_ = loading;
    ++collection_revision_;
    sniffed_.clear();
    fs::ModelChange change{};
    change.loaded = !loading;
    on_change(change);
}

bool Folder::update_collection_rows(std::vector<std::pair<std::size_t, fs::Entry>> rows) {
    if (!collection_)
        return false;
    for (const auto& [index, entry] : rows)
        if (index >= collection_entries_.size() || collection_entries_[index].key() != entry.key())
            return false;
    if (rows.empty())
        return true;
    for (auto& [index, entry] : rows) {
        sniffed_.erase(entry.key());
        collection_entries_[index] = std::move(entry);
    }
    ++collection_revision_;
    selection_changed(false);
    return true;
}

void Folder::append_collection(std::vector<fs::Entry> rows, std::size_t total, bool loading) {
    if (!collection_)
        return;
    const auto start = collection_entries_.size();
    collection_entries_.insert(collection_entries_.end(), std::make_move_iterator(rows.begin()),
                               std::make_move_iterator(rows.end()));
    collection_total_ = total;
    collection_loading_ = loading;
    ++collection_revision_;
    // A restored cursor may be waiting for a later page; existing indices never move.
    if (!cursor_index_ && !cursor_name_.empty()) {
        for (std::size_t i = start; i < collection_entries_.size(); ++i) {
            if (collection_entries_[i].key() == cursor_name_) {
                cursor_index_ = i;
                cursor_expected_ = false;
                reveal_ = Reveal::jump;
                break;
            }
        }
    }
    selection_changed(false);
}

Folder::~Folder() {
    *alive_ = false;
    media_job_.cancel();
}

void Folder::notify_visible_range(std::size_t first, std::size_t last_exclusive) const {
    if (!collection_ || !visible_range_handler_)
        return;
    first = std::min(first, collection_entries_.size());
    last_exclusive = std::clamp(last_exclusive, first, collection_entries_.size());
    // The controller may replace the snapshot or its handler while handling the notification.
    const auto handler = visible_range_handler_;
    handler(first, last_exclusive);
}

const fs::Entry* Folder::cursor_entry() const {
    return cursor_index_ ? &entries()[*cursor_index_] : nullptr;
}

const fs::Entry* Folder::single_selection() const {
    const fs::Entry* const entry = cursor_entry();
    if (entry == nullptr || selected_.size() != 1 || !selected_.contains(entry->key())) {
        return nullptr;
    }
    return entry;
}

std::vector<std::string> Folder::selected_names() const {
    std::vector<std::string> names;
    names.reserve(selected_.size());
    for (const fs::Entry& entry : entries()) {
        if (selected_.contains(entry.key())) {
            names.push_back(entry.key());
        }
    }
    return names;
}

std::vector<const fs::Entry*> Folder::selected_entries() const {
    std::vector<const fs::Entry*> rows;
    for (const auto& entry : entries())
        if (is_selected(entry.key()))
            rows.push_back(&entry);
    return rows;
}

std::vector<std::filesystem::path> Folder::selected_paths() const {
    std::vector<std::filesystem::path> paths;
    for (const auto* entry : selected_entries()) {
        auto path = path_of(*entry);
        if (!path.empty())
            paths.push_back(std::move(path));
    }
    return paths;
}

const SelectionSummary& Folder::summary() const {
    if (!summary_) {
        SelectionSummary summary;
        for (const fs::Entry& entry : entries()) {
            if (!selected_.contains(entry.key())) {
                continue;
            }
            if (entry.is_directory()) {
                ++summary.folders;
            } else {
                ++summary.files;
                summary.bytes += entry.size;
            }
        }
        summary_ = summary;
    }
    return *summary_;
}

Folder::Reveal Folder::take_reveal() noexcept {
    const Reveal reveal = cursor_index_ ? reveal_ : Reveal::none;
    reveal_ = Reveal::none;
    return reveal;
}

void Folder::selection_changed(bool feedback) {
    if (feedback)
        ++selection_revision_;
    summary_.reset();
    redraw_();
    if (changed_) {
        changed_();
    }
}

void Folder::set_order(const fs::SortOrder& order) {
    if (order == this->order()) {
        return;  // already so: no re-sort, no jump
    }
    if (collection_) {
        collection_order_ = order;
        redraw_();
        return;
    }
    model_.set_order(order);
    reveal_ = Reveal::jump;
    redraw_();
}

void Folder::request_media(std::size_t first, std::size_t last) {
    // External collections own their metadata; DirectoryModel cannot publish their rows.
    if (collection_ || is_archive())
        return;
    last = std::min(last, entries().size());
    for (std::size_t i = first; i < last && media_queue_.size() < 256; ++i) {
        const auto& entry = entries()[i];
        if (entry.is_directory() || entry.media || media_queued_.contains(entry.name))
            continue;
        const auto type = mime_of(entry);
        const auto mime =
            std::string{mime_->name(type == fs::kUnknownMime ? entry.name_guess : type)};
        if (!mime.starts_with("image/") && !mime.starts_with("video/") &&
            !mime.starts_with("audio/"))
            continue;
        media_queued_.insert(entry.name);
        // Retain only identity and requested type, not unrelated entry strings.
        fs::Entry identity;
        identity.name = entry.name;
        identity.size = entry.size;
        identity.modified_ns = entry.modified_ns;
        media_queue_.push_back(
            {std::move(identity), path_of(entry), mime, media_epochs_[entry.name]});
    }
    pump_media();
}

void Folder::pump_media() {
    if (media_running_ || media_queue_.empty())
        return;
    std::vector<MediaWork> batch;
    while (!media_queue_.empty() && batch.size() < 8) {
        batch.push_back(std::move(media_queue_.front()));
        media_queue_.pop_front();
    }
    media_running_ = true;
    media_job_ = pool_.submit(
        core::Priority::nearby,
        [batch = std::move(batch)](const core::CancelToken& token) mutable {
            for (auto& item : batch) {
                if (token.cancelled())
                    break;
                const auto info = picture::load_photo_info(item.path, item.mime);
                auto media = std::make_shared<fs::MediaMetadata>();
                media->width = info.width;
                media->height = info.height;
                media->duration_ms = info.duration_ms;
                media->bitrate_bps = info.bitrate_bps;
                media->video_codec = info.video_codec;
                media->audio_codec = info.audio_codec;
                item.entry.media = std::move(media);
            }
            return batch;
        },
        [this, alive = alive_](std::vector<MediaWork> completed) {
            if (!*alive)
                return;
            media_running_ = false;
            std::vector<fs::Entry> results;
            for (auto& item : completed) {
                const auto epoch = media_epochs_.find(item.entry.name);
                if (epoch != media_epochs_.end() && epoch->second == item.epoch && item.entry.media)
                    results.push_back(item.entry);
                media_queued_.erase(item.entry.name);
                media_epochs_.erase(item.entry.name);
            }
            model_.apply_media(results);
            pump_media();
            redraw_();
        });
}

void Folder::on_change(const fs::ModelChange& change) {
    if (!change.metadata_only) {
        if (change.reset) {
            // Only queued/in-flight identities own epochs. Keep in-flight reservations
            // until their callbacks discard the old generation; clear abandoned queue keys.
            for (auto& [name, epoch] : media_epochs_)
                ++epoch;
            for (const auto& item : media_queue_) {
                media_epochs_.erase(item.entry.name);
                media_queued_.erase(item.entry.name);
            }
            media_queue_.clear();
        }
        const auto invalidate = [this](const std::string& name) {
            if (auto it = media_epochs_.find(name); it != media_epochs_.end())
                ++it->second;
        };
        for (const auto& name : change.updated)
            invalidate(name);
        for (const auto& name : change.removed)
            invalidate(name);
        for (const auto& [old_name, new_name] : change.renamed) {
            invalidate(old_name);
            invalidate(new_name);
        }
    }
    for (const std::string& name : change.removed) {
        sniffed_.erase(name);
        selected_.erase(name);
    }
    for (const std::string& name : change.updated) {
        if (!change.metadata_only)
            sniffed_.erase(name);  // the content may be different now
    }
    for (const auto& [from, to] : change.renamed) {
        sniffed_.erase(from);
        if (selected_.erase(from) > 0) {
            selected_.insert(to);
        }
        if (from == cursor_name_) {
            cursor_name_ = to;
        }
        if (from == anchor_) {
            anchor_ = to;
        }
    }

    const bool had_cursor = !cursor_name_.empty();
    const bool had_selection = !selected_.empty();
    // A reset (reload after lost events) is followed by a new load: wait for it.
    const bool final_listing = model_.error() || (!change.reset && !loading());
    const std::vector<fs::Entry>& list = entries();
    const std::optional<std::size_t> before = cursor_index_;
    cursor_index_.reset();
    if (had_cursor) {
        const auto it = std::ranges::find(list, cursor_name_, &fs::Entry::key);
        if (it != list.end()) {
            cursor_index_ = static_cast<std::size_t>(it - list.begin());
            if (!before) {
                reveal_ = Reveal::jump;  // it just landed
            }
        } else if (final_listing && !cursor_expected_) {
            cursor_name_.clear();  // gone, or it never came
        }
        if (cursor_index_) {
            cursor_expected_ = false;
        }
    }
    if (had_selection && final_listing && (change.loaded || model_.error())) {
        // After a (re)load, names that did not come back are gone.
        std::unordered_set<std::string> present;
        for (const fs::Entry& entry : list) {
            if (selected_.contains(entry.key())) {
                present.insert(entry.key());
            }
        }
        selected_ = std::move(present);
    }
    if (had_cursor || had_selection) {
        selection_changed(false);  // Metadata/scan updates do not replay selection feedback.
    } else {
        redraw_();
    }
}

void Folder::set_cursor(std::optional<std::size_t> index, SelectMode mode) {
    cursor_expected_ = false;
    const std::vector<fs::Entry>& list = entries();
    if (!index || list.empty()) {
        reveal_ = Reveal::none;
        if (cursor_name_.empty() && selected_.empty()) {
            return;
        }
        cursor_name_.clear();
        cursor_index_.reset();
        anchor_.clear();
        selected_.clear();
        selection_changed();
        return;
    }
    const std::size_t target = std::min(*index, list.size() - 1);
    const std::string& name = list[target].key();
    switch (mode) {
        case SelectMode::replace:
            selected_ = {name};
            anchor_ = name;
            break;
        case SelectMode::extend: {
            const auto anchor = std::ranges::find(list, anchor_, &fs::Entry::key);
            const std::size_t from = anchor != list.end()
                                         ? static_cast<std::size_t>(anchor - list.begin())
                                         : cursor_index_.value_or(target);
            anchor_ = list[from].key();
            selected_.clear();
            for (std::size_t i = std::min(from, target); i <= std::max(from, target); ++i) {
                selected_.insert(list[i].key());
            }
            break;
        }
        case SelectMode::keep:
            break;
        case SelectMode::toggle:
            if (selected_.erase(name) == 0) {
                selected_.insert(name);
            }
            anchor_ = name;
            break;
    }
    cursor_name_ = name;
    cursor_index_ = target;
    reveal_ = Reveal::glide;
    selection_changed();
}

void Folder::press(std::optional<std::size_t> index, SelectMode mode) {
    press_pending_ = false;
    if (index && *index < entries().size() && mode == SelectMode::replace && selected_.size() > 1 &&
        selected_.contains(entries()[*index].key())) {
        set_cursor(index, SelectMode::keep);
        press_pending_ = true;
        return;
    }
    set_cursor(index, mode);
}

void Folder::select_band(const std::vector<std::string>& base, std::span<const std::size_t> rows) {
    press_pending_ = false;
    std::unordered_set<std::string> chosen{base.begin(), base.end()};
    const std::vector<fs::Entry>& list = entries();
    for (const std::size_t row : rows) {
        if (row < list.size()) {
            chosen.insert(list[row].key());
        }
    }
    if (chosen == selected_) {
        return;
    }
    selected_ = std::move(chosen);
    selection_changed();
}

void Folder::release_press() {
    if (press_pending_) {
        press_pending_ = false;
        set_cursor(cursor_index_, SelectMode::replace);
    }
}

void Folder::move_cursor(std::ptrdiff_t delta, SelectMode mode, bool wrap) {
    const std::size_t count = entries().size();
    if (count == 0) {
        return;
    }
    if (!cursor_index_) {
        set_cursor(0, mode == SelectMode::keep ? SelectMode::keep : SelectMode::replace);
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(count) - 1;
    auto target = static_cast<std::ptrdiff_t>(*cursor_index_) + delta;
    target = wrap ? ((target % (last + 1)) + last + 1) % (last + 1)
                  : std::clamp(target, std::ptrdiff_t{0}, last);
    set_cursor(static_cast<std::size_t>(target), mode);
}

void Folder::expect_name(std::string name) {
    select_name(name);
    if (cursor_index_) {
        return;  // already listed
    }
    cursor_name_ = std::move(name);
    cursor_expected_ = true;
    selected_ = {cursor_name_};
    anchor_ = cursor_name_;
    selection_changed();
}

void Folder::expect_names(const std::vector<std::string>& names) {
    if (names.empty()) {
        return;
    }
    expect_name(names.front());
    // Selected by name: the ones not listed yet are selected when they arrive.
    selected_.insert(names.begin(), names.end());
    selection_changed();
}

void Folder::select_name(std::string name) {
    if (!shows_hidden() && hidden_name(name)) {
        model_.set_filter(false, name);  // asked for by name: shown all the same
    }
    cursor_expected_ = false;
    cursor_name_ = std::move(name);
    cursor_index_.reset();
    const std::vector<fs::Entry>& list = entries();
    const auto it = std::ranges::find(list, cursor_name_, &fs::Entry::key);
    if (it != list.end()) {
        cursor_index_ = static_cast<std::size_t>(it - list.begin());
    } else if (!loading()) {
        cursor_name_.clear();
    }
    selected_.clear();
    if (!cursor_name_.empty()) {
        selected_.insert(cursor_name_);
    }
    anchor_ = cursor_name_;
    reveal_ = Reveal::jump;  // now, or when it lands
    selection_changed();
}

void Folder::select_all() {
    const std::vector<fs::Entry>& list = entries();
    if (list.empty()) {
        return;
    }
    for (const fs::Entry& entry : list) {
        selected_.insert(entry.key());
    }
    if (!cursor_index_) {
        cursor_index_ = 0;
        cursor_name_ = list.front().key();
        anchor_ = cursor_name_;
    }
    selection_changed();
}

void Folder::invert_selection() {
    const std::vector<fs::Entry>& list = entries();
    if (list.empty()) {
        return;
    }
    // Names selected but not listed yet (pasted, on their way) count as selected, so they drop out.
    std::unordered_set<std::string> inverted;
    for (const fs::Entry& entry : list) {
        if (!selected_.contains(entry.key())) {
            inverted.insert(entry.key());
        }
    }
    selected_ = std::move(inverted);
    press_pending_ = false;
    if (!cursor_index_) {
        const auto first = std::ranges::find_if(
            list, [this](const fs::Entry& entry) { return selected_.contains(entry.key()); });
        if (first != list.end()) {
            cursor_index_ = static_cast<std::size_t>(first - list.begin());
            cursor_name_ = first->key();
            cursor_expected_ = false;
            anchor_ = cursor_name_;
        }
    }
    selection_changed();
}

std::vector<std::size_t> Folder::matching(const std::string& glob) const {
    std::vector<std::size_t> rows;
    if (glob.empty()) {
        return rows;
    }
    const std::vector<fs::Entry>& list = entries();
    const fs::NamePattern pattern{glob};
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (pattern.matches(list[i].name)) {
            rows.push_back(i);
        }
    }
    return rows;
}

void Folder::select_rows(std::span<const std::size_t> rows, bool add) {
    const std::vector<fs::Entry>& list = entries();
    std::unordered_set<std::string> chosen;
    if (add) {
        chosen = selected_;
    }
    std::optional<std::size_t> first;
    bool cursor_in = false;
    for (const std::size_t row : rows) {
        if (row >= list.size()) {
            continue;
        }
        chosen.insert(list[row].key());
        first = first.value_or(row);
        cursor_in = cursor_in || row == cursor_index_;
    }
    if (!first) {
        return;  // nothing matched: the selection stays
    }
    press_pending_ = false;
    selected_ = std::move(chosen);
    if (!cursor_in) {
        cursor_expected_ = false;
        cursor_index_ = first;
        cursor_name_ = list[*first].key();
        reveal_ = Reveal::jump;
    }
    anchor_ = cursor_name_;
    selection_changed();
}

void Folder::set_show_hidden(bool show, bool keep_cursor) {
    if (collection_ || show == shows_hidden()) {
        return;
    }
    std::string keep;
    std::string next;  // where the cursor goes when its entry is left out
    if (const fs::Entry* const entry = cursor_entry();
        !show && entry != nullptr && entry->hidden()) {
        if (keep_cursor) {
            keep = entry->key();
        } else {
            const std::vector<fs::Entry>& list = entries();
            const auto shown = [](const fs::Entry& candidate) { return !candidate.hidden(); };
            const auto at = list.begin() + (entry - list.data());
            if (const auto after = std::find_if(at, list.end(), shown); after != list.end()) {
                next = after->key();
            } else if (const auto before =
                           std::find_if(std::make_reverse_iterator(at), list.rend(), shown);
                       before != list.rend()) {
                next = before->key();
            }
        }
    }
    model_.set_filter(show, std::move(keep));
    if (!next.empty()) {
        // The rest of the selection stays; alone, the cursor's entry is selected.
        const std::vector<fs::Entry>& list = entries();
        const auto it = std::ranges::find(list, next, &fs::Entry::key);
        set_cursor(static_cast<std::size_t>(it - list.begin()),
                   selected_.empty() ? SelectMode::replace : SelectMode::keep);
    }
}

void Folder::collapse_selection() {
    selected_.clear();
    if (cursor_index_) {
        selected_.insert(cursor_name_);
        anchor_ = cursor_name_;
    }
    selection_changed();
}

fs::MimeId Folder::mime_of(const fs::Entry& entry) const {
    if (entry.mime != fs::kUnknownMime) {
        return entry.mime;
    }
    const auto it = sniffed_.find(entry.key());
    return it == sniffed_.end() ? fs::kUnknownMime : it->second;
}

bool Folder::type_runs(const fs::Entry& entry) const {
    if (entry.kind != fs::EntryKind::file) {
        return false;
    }
    const fs::MimeId type = mime_of(entry);
    if (type == fs::kUnknownMime) {
        return true;
    }
    constexpr std::array<std::string_view, 2> running_types{"application/x-executable",
                                                            "application/x-sharedlib"};
    return std::ranges::any_of(running_types, [&](std::string_view name) {
        const fs::MimeId parent = mime_->find(name);
        return parent != fs::kUnknownMime && mime_->is_a(type, parent);
    });
}

bool Folder::runs_as_program(const fs::Entry& entry) const {
    return !collection_ && entry.available && (entry.mode & S_IXUSR) != 0 && !is_trash() &&
           !is_archive() && type_runs(entry);
}

std::filesystem::path Folder::path_of(const fs::Entry& entry) const {
    if (collection_)
        return entry.location;
    if (const fs::TrashedItem* const item = model_.trashed(entry.name)) {
        return item->path;
    }
    return path() / entry.name;
}

void Folder::sniff(std::size_t first, std::size_t last) {
    const std::vector<fs::Entry>& list = entries();
    // Each entry with where it really is (the trash folder's are elsewhere).
    std::vector<std::pair<fs::Entry, std::filesystem::path>> batch;
    for (std::size_t i = first; i < std::min(last, list.size()) && batch.size() < kMaxSniffBatch;
         ++i) {
        const fs::Entry& entry = list[i];
        if (entry.can_preview() && (!collection_ || !entry.location.empty()) &&
            entry.mime == fs::kUnknownMime && !sniffed_.contains(entry.key()) &&
            !sniffing_.contains(entry.key())) {
            sniffing_.insert(entry.key());
            batch.emplace_back(entry, path_of(entry));
        }
    }
    if (batch.empty()) {
        return;
    }
    pool_.submit(
        core::Priority::visible,
        [mime = mime_, batch](const core::CancelToken& token) {
            std::vector<std::pair<std::string, fs::MimeId>> types;
            types.reserve(batch.size());
            for (const auto& [entry, path] : batch) {
                if (token.cancelled()) {
                    break;
                }
                fs::Entry real = entry;
                real.name = path.filename().string();
                types.emplace_back(entry.key(), mime->detect(path.parent_path(), real));
            }
            return types;
        },
        [this, alive = alive_](std::vector<std::pair<std::string, fs::MimeId>> types) {
            if (!*alive) {
                return;  // the folder was closed
            }
            for (auto& [name, type] : types) {
                sniffing_.erase(name);
                sniffed_[std::move(name)] = type;
            }
            redraw_();
        });
}

}  // namespace rummage::views
