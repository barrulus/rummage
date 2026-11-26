#include "rummage/fs/directory_model.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::fs {
namespace {

ModelChange reset_change() {
    ModelChange change;
    change.reset = true;
    return change;
}

ModelChange loaded_change() {
    ModelChange change;
    change.loaded = true;
    return change;
}

}  // namespace

DirectoryModel::DirectoryModel(core::EventLoop& loop, core::WorkerPool& pool,
                               DirectoryWatcher& watcher, std::shared_ptr<const MimeDatabase> mime,
                               std::chrono::milliseconds settle)
    : loop_{loop}, pool_{pool}, watcher_{watcher}, settle_{settle} {
    options_.mime = std::move(mime);
}

DirectoryModel::~DirectoryModel() { stop(); }

void DirectoryModel::open(std::filesystem::path path) {
    stop();
    path_ = std::move(path);
    listing_.clear();
    names_.clear();
    error_.clear();
    notify(reset_change());

    if (is_recent()) {
        loading_ = true;
        load_recent();
        return;
    }
    if (fs::is_trash(path_)) {
        trash_ = true;
        loading_ = true;
        load_trash();
        return;
    }
    loading_ = true;
    watch_ = watcher_.watch_async(
        pool_, path_, [this](const std::vector<DirectoryEvent>& events) { on_events(events); },
        [this](std::error_code error) {
            if (error == std::errc::not_a_directory) {
                load_archive();  // archive path classification also stays off the input thread
                return;
            }
            if (error == std::errc::no_space_on_device) {
                log::warn("{}: cannot watch for changes (inotify watch limit reached)",
                          path_.string());
            } else if (error) {
                fail(error);
                return;
            }
            start_load();
        });
    if (watch_ == 0) {
        log::warn("{}: cannot watch for changes (pending watch limit reached)", path_.string());
        start_load();
    }
}

void DirectoryModel::set_order(SortOrder order) {
    listing_.set_order(order);
    notify(reset_change());
}

void DirectoryModel::refresh_entry(const std::string& name) {
    if (is_recent()) {
        if (refresh_running_)
            recent_again_ = true;
        else if (!settle_timer_)
            settle_timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
                settle_timer_ = 0;
                load_recent();
            });
        return;
    }
    dirty_.insert(name);
    if (!loading_)
        schedule_refresh();
}

void DirectoryModel::set_filter(bool show_hidden, std::string keep) {
    Listing::Moved moved = listing_.set_filter(show_hidden, std::move(keep));
    if (moved.hidden.empty() && moved.shown.empty()) {
        return;
    }
    ModelChange change;
    change.removed = std::move(moved.hidden);
    change.added = std::move(moved.shown);
    notify(change);
}

void DirectoryModel::set_predicate(Listing::Predicate predicate) {
    auto moved = listing_.set_predicate(std::move(predicate));
    if (moved.hidden.empty() && moved.shown.empty()) {
        return;
    }
    ModelChange change;
    change.removed = std::move(moved.hidden);
    change.added = std::move(moved.shown);
    notify(change);
}

void DirectoryModel::start_load() {
    loading_ = true;
    load_ = load_directory(
        pool_, path_,
        [this](std::vector<Entry> chunk) {
            ModelChange change;
            change.added.reserve(chunk.size());
            for (const Entry& entry : chunk) {
                names_.insert(entry.name);
                change.added.push_back(entry.name);
            }
            listing_.add(std::move(chunk));
            notify(change);
        },
        [this](const ReadResult& result) {
            loading_ = false;
            if (result.error) {
                fail(result.error);
                return;
            }
            notify(loaded_change());
            if (!dirty_.empty()) {
                schedule_refresh();  // changes that happened while loading
            }
        },
        options_);
}

void DirectoryModel::stop() {
    load_.cancel();
    refresh_job_.cancel();
    refresh_running_ = false;
    loop_.remove(settle_timer_);
    settle_timer_ = 0;
    watcher_.unwatch(watch_);
    watch_ = 0;
    loading_ = false;
    dirty_.clear();
    moved_from_.clear();
    renames_.clear();
    for (const auto& [dir, id] : trash_watches_) {
        watcher_.unwatch(id);
    }
    trash_watches_.clear();
    recent_paths_.clear();
    recent_again_ = false;
    trashed_.clear();
    trash_ = false;
    trash_again_ = false;
    archive_tree_.reset();
    archive_at_ = {};
    problem_.clear();
}

std::string DirectoryModel::problem() const {
    if (!problem_.empty()) {
        return problem_;
    }
    return error_ ? error_.message() : std::string{};
}

const archive::Entry* DirectoryModel::archive_entry(const std::string& name) const {
    if (!archive_tree_) {
        return nullptr;
    }
    return archive_tree_->entry(archive_at_.inner.empty() ? name : archive_at_.inner + "/" + name);
}

void DirectoryModel::load_archive() {
    struct Found {
        std::optional<ArchiveLocation> where;
        std::shared_ptr<const ArchiveTree> tree;
    };

    loading_ = true;
    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, mime = options_.mime](const core::CancelToken& /*token*/) {
            Found found;
            found.where = find_archive(path);
            if (found.where) {
                found.tree = fs::archive_tree(found.where->archive, mime.get());
            }
            return found;
        },
        [this](Found found) {
            refresh_running_ = false;
            loading_ = false;
            if (!found.where || !found.tree) {
                fail(std::make_error_code(std::errc::not_a_directory));
                return;
            }
            const ArchiveTree& tree = *found.tree;
            const std::vector<Entry>* entries = tree.folder(found.where->inner);
            const bool failed = tree.failure() != archive::Failure::none;
            if (entries == nullptr || (failed && entries->empty())) {
                fail_with(std::make_error_code(std::errc::io_error),
                          failed ? tree.detail() : std::string{"no such folder in the archive"});
                return;
            }
            archive_tree_ = std::move(found.tree);
            archive_at_ = std::move(*found.where);
            if (failed) {
                problem_ = "damaged: " + archive_tree_->detail();
            } else if (const std::size_t out = archive_tree_->left_out(); out > 0) {
                problem_ = std::format("{} {} with unsafe names left out", out,
                                       out == 1 ? "entry" : "entries");
            }
            ModelChange change;
            change.added.reserve(entries->size());
            for (const Entry& entry : *entries) {
                names_.insert(entry.name);
                change.added.push_back(entry.name);
            }
            listing_.add(std::vector<Entry>{*entries});
            notify(change);
            notify(loaded_change());
        });
}

const TrashedItem* DirectoryModel::trashed(const std::string& name) const {
    const auto it = trashed_.find(name);
    return it == trashed_.end() ? nullptr : &it->second;
}

struct DirectoryModel::TrashListing {
    std::vector<TrashedItem> items;
    std::vector<Entry> entries;  // items[i]'s, named as listed
    std::vector<std::filesystem::path> info_dirs;
};

std::filesystem::path DirectoryModel::source_path(const std::string& name) const {
    if (const auto found = recent_paths_.find(name); found != recent_paths_.end())
        return found->second;
    return path_ / name;
}

void DirectoryModel::load_recent() {
    struct Result {
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::filesystem::path> paths;
        std::unordered_set<std::string> directories;
    };

    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [options = options_](const core::CancelToken& token) {
            Result result;
            const auto history = RecentFiles::default_directory();
            // Creating the history directory also permits watching an initially empty history.
            std::error_code error;
            std::filesystem::create_directories(history, error);
            result.directories.insert(history.string());
            for (const auto& recent : RecentFiles{}.load(true)) {
                if (token.cancelled())
                    break;
                result.directories.insert(recent.path.parent_path().string());
                if (::access(recent.path.c_str(), R_OK) != 0)
                    continue;
                auto read = read_entries(recent.path.parent_path(),
                                         {recent.path.filename().string()}, options);
                if (read.empty() || !read.front())
                    continue;
                auto entry = std::move(*read.front());
                if (entry.kind != EntryKind::file)
                    continue;
                // Preserve a stable, unambiguous identity across equal basenames. The source
                // stays separate: virtual display names must never be used for filesystem I/O.
                entry.display_name = entry.name;
                entry.name += " — " + recent.path.parent_path().string();
                result.paths.emplace(entry.name, recent.path);
                result.directories.insert(recent.path.parent_path().string());
                result.entries.push_back(std::move(entry));
            }
            return result;
        },
        [this](Result result) {
            refresh_running_ = false;
            for (auto it = trash_watches_.begin(); it != trash_watches_.end();) {
                if (!result.directories.contains(it->first)) {
                    watcher_.unwatch(it->second);
                    it = trash_watches_.erase(it);
                } else
                    ++it;
            }
            for (const auto& directory : result.directories) {
                if (trash_watches_.contains(directory))
                    continue;
                std::error_code error;
                const bool history = directory == RecentFiles::default_directory().string();
                const auto id = watcher_.watch(
                    directory,
                    [this, history](const auto& events) {
                        // Ignore writers' lock files and temporary files; only actual
                        // history/enable-marker changes need another read.
                        if (!history || std::ranges::any_of(events, [](const auto& event) {
                                return event.name == "recent-files" ||
                                       event.name == "recent-disabled" ||
                                       event.kind == DirectoryEvent::Kind::overflow ||
                                       event.kind == DirectoryEvent::Kind::directory_gone;
                            }))
                            refresh_entry({});
                    },
                    error);
                if (id)
                    trash_watches_.emplace(directory, id);
            }
            ModelChange change;
            std::unordered_set<std::string> gone;
            for (const auto& name : names_)
                if (!result.paths.contains(name)) {
                    gone.insert(name);
                    change.removed.push_back(name);
                }
            for (const auto& entry : result.entries)
                (names_.contains(entry.name) ? change.updated : change.added).push_back(entry.name);
            listing_.replace(gone, std::move(result.entries));
            recent_paths_ = std::move(result.paths);
            names_.clear();
            for (const auto& [name, path] : recent_paths_)
                names_.insert(name);
            change.loaded = std::exchange(loading_, false);
            notify(change);
            if (std::exchange(recent_again_, false))
                load_recent();
        });
}

void DirectoryModel::load_trash() {
    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [sources = trash_sources(), options = options_](const core::CancelToken& /*token*/) {
            // The home trash is made when missing (the specification asks for it), so it can be
            // watched from the start.
            if (!sources.home.empty()) {
                std::error_code ignored;
                std::filesystem::create_directories(sources.home / "files", ignored);
                std::filesystem::create_directories(sources.home / "info", ignored);
            }
            TrashListing listing;
            for (const TrashDir& trash : existing_trashes(sources)) {
                listing.info_dirs.push_back(trash.path / "info");
            }
            std::vector<TrashedItem> items = list_trash(sources);
            for (TrashedItem& item : items) {
                std::vector<std::optional<Entry>> read =
                    read_entries(item.path.parent_path(), {item.path.filename().string()}, options);
                if (read.empty() || !read.front()) {
                    continue;  // gone meanwhile
                }
                Entry entry = std::move(*read.front());
                entry.name = item.name;
                listing.entries.push_back(std::move(entry));
                listing.items.push_back(std::move(item));
            }
            return listing;
        },
        [this](TrashListing listing) {
            refresh_running_ = false;
            apply_trash(std::move(listing));
            if (std::exchange(trash_again_, false)) {
                load_trash();
            }
        });
}

void DirectoryModel::apply_trash(TrashListing listing) {
    // Watch the info/ folders: an item comes or goes with its .trashinfo.
    std::unordered_map<std::string, DirectoryWatcher::WatchId> watches;
    for (const std::filesystem::path& dir : listing.info_dirs) {
        const std::string key = dir.string();
        if (const auto it = trash_watches_.find(key); it != trash_watches_.end()) {
            watches.emplace(key, it->second);
            trash_watches_.erase(it);
            continue;
        }
        const DirectoryWatcher::WatchId id =
            watcher_.watch_async(pool_, dir, [this](const std::vector<DirectoryEvent>& /*events*/) {
                if (refresh_running_) {
                    trash_again_ = true;
                    return;
                }
                if (settle_timer_ == 0) {
                    settle_timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
                        settle_timer_ = 0;
                        load_trash();
                    });
                }
            });
        if (id != 0) {
            watches.emplace(key, id);
        }
    }
    for (const auto& [dir, id] : trash_watches_) {
        watcher_.unwatch(id);  // a trash that went away (a disk unmounted)
    }
    trash_watches_ = std::move(watches);

    // What changed, by name.
    std::unordered_map<std::string_view, const Entry*> listed;
    listed.reserve(listing_.entries().size() + listing_.hidden().size());
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            listed.emplace(entry.name, &entry);
        }
    }
    std::unordered_map<std::string, TrashedItem> items;
    ModelChange change;
    std::vector<Entry> present;
    for (std::size_t i = 0; i < listing.items.size(); ++i) {
        Entry& entry = listing.entries[i];
        const std::string name = entry.name;
        const auto before = listed.find(name);
        const TrashedItem* const was = trashed(name);
        if (before == listed.end()) {
            change.added.push_back(name);
            present.push_back(std::move(entry));
        } else if (was == nullptr || was->path != listing.items[i].path ||
                   before->second->size != entry.size ||
                   before->second->modified_ns != entry.modified_ns ||
                   before->second->kind != entry.kind) {
            change.updated.push_back(name);
            present.push_back(std::move(entry));
        }
        items.emplace(name, std::move(listing.items[i]));
    }
    std::unordered_set<std::string> gone;
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            if (!items.contains(entry.name)) {
                gone.insert(entry.name);
                change.removed.push_back(entry.name);
            }
        }
    }
    listing_.replace(gone, std::move(present));
    trashed_ = std::move(items);
    names_.clear();
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            names_.insert(entry.name);
        }
    }
    const bool first = loading_;
    loading_ = false;
    if (!change.added.empty() || !change.removed.empty() || !change.updated.empty()) {
        notify(change);
    }
    if (first) {
        notify(loaded_change());
    }
}

void DirectoryModel::on_events(const std::vector<DirectoryEvent>& events) {
    using Kind = DirectoryEvent::Kind;
    for (const DirectoryEvent& event : events) {
        switch (event.kind) {
            case Kind::directory_gone:
                fail(std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            case Kind::overflow:
                // Events were lost: read the whole directory again.
                load_.cancel();
                refresh_job_.cancel();
                refresh_running_ = false;
                loop_.remove(settle_timer_);
                settle_timer_ = 0;
                dirty_.clear();
                moved_from_.clear();
                renames_.clear();
                listing_.clear();
                names_.clear();
                notify(reset_change());
                start_load();
                return;
            case Kind::moved_from:
                moved_from_[event.cookie] = event.name;
                dirty_.insert(event.name);
                break;
            case Kind::moved_to:
                if (const auto it = moved_from_.find(event.cookie); it != moved_from_.end()) {
                    renames_.emplace_back(it->second, event.name);
                    moved_from_.erase(it);
                }
                dirty_.insert(event.name);
                break;
            case Kind::created:
            case Kind::deleted:
            case Kind::changed:
                dirty_.insert(event.name);
                break;
        }
    }
    if (!loading_) {
        schedule_refresh();
    }
}

void DirectoryModel::schedule_refresh() {
    if (settle_timer_ != 0 || refresh_running_) {
        return;  // already coming; a running refresh reschedules when it lands
    }
    settle_timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
        settle_timer_ = 0;
        refresh();
    });
}

void DirectoryModel::refresh() {
    if (dirty_.empty()) {
        return;
    }
    std::vector<std::string> names(dirty_.begin(), dirty_.end());
    std::vector<std::pair<std::string, std::string>> renames = std::move(renames_);
    dirty_.clear();
    renames_.clear();
    moved_from_.clear();  // a move away whose pair never came: just a removal
    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, names, options = options_](const core::CancelToken& /*token*/) {
            return read_entries(path, names, options);
        },
        [this, names, renames = std::move(renames)](std::vector<std::optional<Entry>> entries) {
            refresh_running_ = false;
            apply(names, std::move(entries), renames);
            if (!dirty_.empty()) {
                schedule_refresh();
            }
        });
}

void DirectoryModel::apply(const std::vector<std::string>& names,
                           std::vector<std::optional<Entry>> entries,
                           const std::vector<std::pair<std::string, std::string>>& renames) {
    ModelChange change;
    std::unordered_set<std::string> gone;
    std::vector<Entry> present;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        if (std::optional<Entry>& entry = entries[i]; entry.has_value()) {
            (names_.contains(name) ? change.updated : change.added).push_back(name);
            names_.insert(name);
            present.push_back(std::move(entry.value()));
        } else if (names_.erase(name) > 0) {
            change.removed.push_back(name);
            gone.insert(name);
        }
    }
    // A rename is a removal of the old name and an addition of the new one in the same batch.
    for (const auto& [from, to] : renames) {
        const auto removed = std::ranges::find(change.removed, from);
        const auto added = std::ranges::find(change.added, to);
        if (removed != change.removed.end() && added != change.added.end()) {
            change.removed.erase(removed);
            change.added.erase(added);
            change.renamed.emplace_back(from, to);
        }
    }
    listing_.replace(gone, std::move(present));
    if (!change.added.empty() || !change.removed.empty() || !change.updated.empty() ||
        !change.renamed.empty()) {
        notify(change);
    }
}

void DirectoryModel::apply_media(const std::vector<Entry>& results) {
    std::unordered_map<std::string, const Entry*> by_name;
    for (const auto& result : results)
        by_name.emplace(result.name, &result);
    std::vector<Entry> changed;
    ModelChange change;
    change.metadata_only = true;
    for (const auto& entry : listing_.entries()) {
        const auto it = by_name.find(entry.name);
        if (it == by_name.end() || entry.size != it->second->size ||
            entry.modified_ns != it->second->modified_ns)
            continue;
        auto value = entry;
        value.media = it->second->media;
        changed.push_back(std::move(value));
        change.updated.push_back(entry.name);
    }
    if (!changed.empty()) {
        listing_.replace({}, std::move(changed));
        notify(change);
    }
}

void DirectoryModel::apply_decorations(
    const std::map<std::string, std::shared_ptr<const Decoration>>& decorations) {
    std::vector<Entry> changed;
    ModelChange change;
    change.metadata_only = true;
    for (const auto& entry : listing_.entries()) {
        const auto at = decorations.find(entry.name);
        auto decoration =
            at == decorations.end() ? std::shared_ptr<const Decoration>{} : at->second;
        if ((!decoration && !entry.decoration) ||
            (decoration && entry.decoration && *decoration == *entry.decoration))
            continue;
        auto value = entry;
        value.decoration = std::move(decoration);
        changed.push_back(std::move(value));
        change.updated.push_back(entry.name);
    }
    if (!changed.empty()) {
        listing_.replace({}, std::move(changed));
        notify(change);
    }
}

void DirectoryModel::notify(const ModelChange& change) {
    ++revision_;
    if (change_handler_) {
        change_handler_(change);
    }
}

void DirectoryModel::fail(std::error_code error) { fail_with(error, {}); }

void DirectoryModel::fail_with(std::error_code error, std::string problem) {
    stop();
    error_ = error;
    problem_ = std::move(problem);
    listing_.clear();
    names_.clear();
    notify(reset_change());
}

}  // namespace rummage::fs
