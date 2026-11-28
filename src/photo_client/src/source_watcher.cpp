#include "rummage/photos/source_watcher.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace rummage::photos {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr std::size_t kMaximumWatches = 4096;
constexpr std::size_t kRegistrationsPerTurn = 16;
constexpr std::size_t kMaximumCandidates = 256;

bool within(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto first = root.begin(), second = path.begin();
    while (first != root.end() && second != path.end() && *first == *second) {
        ++first;
        ++second;
    }
    return first == root.end();
}

std::filesystem::path normalized(std::filesystem::path path) {
    path = path.lexically_normal();
    if (path != path.root_path() && path.filename().empty())
        path = path.parent_path();
    return path;
}
}  // namespace

SourceWatcher::SourceWatcher(core::EventLoop& loop, core::WorkerPool& pool,
                             fs::DirectoryWatcher& watcher, std::shared_ptr<Client> client,
                             Changed changed)
    : loop_(loop),
      pool_(pool),
      watcher_(watcher),
      client_(std::move(client)),
      changed_(std::move(changed)) {
    if (!client_ || !client_->available()) {
        stopped_ = true;
        return;
    }
    availability_timer_ = loop_.add_timer(30s, 30s, [this] { refresh_subscriptions(); });
    refresh_subscriptions();
}

SourceWatcher::~SourceWatcher() { stop(); }

void SourceWatcher::stop() {
    stopped_ = true;
    page_job_.cancel();
    refresh_job_.cancel();
    loop_.remove(register_timer_);
    loop_.remove(debounce_timer_);
    loop_.remove(availability_timer_);
    register_timer_ = debounce_timer_ = availability_timer_ = 0;
    for (const auto& [path, registration] : registrations_) {
        (void)path;
        watcher_.unwatch(registration.watch);
    }
    registrations_.clear();
    roots_.clear();
    known_roots_.clear();
    online_.clear();
    pending_paths_.clear();
    pending_sources_.clear();
    candidates_.clear();
    listing_ = false;
    pending_registrations_ = 0;
}

bool SourceWatcher::excluded(const std::filesystem::path& path) const {
    return std::any_of(exclusions_.begin(), exclusions_.end(),
                       [&](const auto& root) { return within(root, path); });
}

void SourceWatcher::refresh_subscriptions() {
    if (stopped_)
        return;
    if (listing_) {
        rerun_ = true;
        return;
    }
    listing_ = true;
    rerun_ = false;
    more_ = false;
    registration_error_.clear();
    limit_reported_ = false;
    ++generation_;
    fetch_page(0);
}

void SourceWatcher::fetch_page(Id after) {
    page_job_ = pool_.submit(
        core::Priority::prefetch,
        [client = client_, after](const core::CancelToken& cancel) {
            return client->request({{"op", "watch_paths"}, {"after_id", after}, {"limit", 128}},
                                   &cancel);
        },
        [this, after](Json response) {
            if (!response.value("ok", false)) {
                const auto error =
                    response.value("error", std::string("Cannot update photo source watches"));
                finish_listing(false);
                if (changed_)
                    changed_(error);
                return;
            }
            try {
                exclusions_.clear();
                for (const auto& path : response.at("internal_exclusions"))
                    exclusions_.push_back(normalized(path.get<std::string>()));
                extensions_ = response.at("extensions").get<std::set<std::string>>();
                std::map<Id, std::filesystem::path> active;
                std::set<Id> online;
                for (const auto& source : response.at("sources")) {
                    const Id id = source.at("id").get<Id>();
                    const auto path = normalized(source.at("path").get<std::string>());
                    if (!source.value("active", false) || !source.value("watch_enabled", true) ||
                        source.value("availability", std::string{}) != "online" || excluded(path))
                        continue;
                    active[id] = path;
                    online.insert(id);
                    if (!online_.contains(id) || !known_roots_.contains(id) ||
                        known_roots_.at(id) != path)
                        pending_sources_.insert(id);
                    known_roots_[id] = path;
                    if (after == 0)
                        pending_paths_.push_back({id, path});
                }
                roots_ = std::move(active);
                online_ = std::move(online);
                for (auto it = registrations_.begin(); it != registrations_.end();) {
                    if (!roots_.contains(it->second.source) ||
                        !within(roots_.at(it->second.source), it->first) || excluded(it->first)) {
                        watcher_.unwatch(it->second.watch);
                        it = registrations_.erase(it);
                    } else
                        ++it;
                }
                for (const auto& path : response.at("paths")) {
                    const Id source = path.at("source_id").get<Id>();
                    const auto directory = normalized(path.at("path").get<std::string>());
                    if (roots_.contains(source) && within(roots_.at(source), directory) &&
                        !excluded(directory))
                        pending_paths_.push_back({source, directory});
                }
                next_page_ = response.at("next_after_id").get<Id>();
                more_ = response.value("more", false);
                if (more_ && next_page_ <= after)
                    throw std::runtime_error("Invalid photo watch pagination");
                register_batch();
            } catch (const std::exception& error) {
                const std::string message = error.what();
                finish_listing(false);
                if (changed_)
                    changed_(message);
            }
        });
}

void SourceWatcher::register_batch() {
    if (stopped_)
        return;
    register_timer_ = 0;
    std::size_t count = 0;
    while (!pending_paths_.empty() && count < kRegistrationsPerTurn) {
        auto item = std::move(pending_paths_.front());
        pending_paths_.pop_front();
        ++count;
        const auto existing = registrations_.find(item.path);
        if (existing != registrations_.end()) {
            existing->second.generation = generation_;
            continue;
        }
        if (registrations_.size() >= kMaximumWatches) {
            if (!limit_reported_) {
                registration_error_ =
                    "Photo Library reached its 4096-folder watch limit. Use Refresh library to "
                    "reconcile unwatched folders, or reduce source scope.";
                limit_reported_ = true;
            }
            continue;
        }
        ++pending_registrations_;
        const auto watch = watcher_.watch_async(
            pool_, item.path,
            [this, source = item.source, path = item.path](const auto& events) {
                receive(source, path, events);
            },
            [this, source = item.source, path = item.path](std::error_code error) {
                if (error) {
                    registrations_.erase(path);
                    if (registration_error_.empty())
                        registration_error_ = "Cannot watch photo folder " + path.string() + ": " +
                                              error.message() +
                                              ". Refresh library remains available.";
                }
                if (!error)
                    pending_sources_.insert(source);  // reconcile events before loop adoption
                if (--pending_registrations_ == 0)
                    register_batch();
            });
        if (watch == 0) {
            --pending_registrations_;
            registration_error_ =
                "Photo Library cannot add more pending folder watches. "
                "Refresh library remains available.";
            continue;
        }
        registrations_.emplace(std::move(item.path), Registration{watch, item.source, generation_});
        // Keep one lookup in flight; a slow mount leaves workers for visible folders.
        break;
    }
    if (pending_registrations_ != 0)
        return;
    if (!pending_paths_.empty()) {
        register_timer_ = loop_.add_timer(10ms, 0ms, [this] { register_batch(); });
        return;
    }
    if (more_) {
        fetch_page(next_page_);
        return;
    }
    const auto error = registration_error_;
    finish_listing(true);
    if (!error.empty() && changed_)
        changed_(error);
}

void SourceWatcher::finish_listing(bool success) {
    if (success)
        for (auto it = registrations_.begin(); it != registrations_.end();) {
            if (it->second.generation != generation_) {
                watcher_.unwatch(it->second.watch);
                it = registrations_.erase(it);
            } else
                ++it;
        }
    pending_paths_.clear();
    listing_ = false;
    schedule_flush();
    if (rerun_)
        refresh_subscriptions();
}

void SourceWatcher::notify_change(Id source, fs::DirectoryEvent::Kind kind) {
    (void)kind;
    if (stopped_ || !roots_.contains(source))
        return;
    pending_sources_.insert(source);
    schedule_flush();
}

void SourceWatcher::receive(Id source, const std::filesystem::path& directory,
                            const std::vector<fs::DirectoryEvent>& events) {
    if (stopped_ || !roots_.contains(source))
        return;
    using Kind = fs::DirectoryEvent::Kind;
    for (const auto& event : events) {
        if (event.kind == Kind::overflow || event.kind == Kind::directory_gone) {
            pending_sources_.insert(source);
            if (event.kind == Kind::directory_gone) {
                const auto found = registrations_.find(directory);
                if (found != registrations_.end()) {
                    watcher_.unwatch(found->second.watch);
                    registrations_.erase(found);
                }
            }
            continue;
        }
        const auto path = normalized(directory / event.name);
        if (excluded(path))
            continue;
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extensions_.contains(extension) || registrations_.contains(path)) {
            pending_sources_.insert(source);
            continue;
        }
        if (event.kind == Kind::created || event.kind == Kind::moved_to) {
            if (candidates_.size() < kMaximumCandidates)
                candidates_.push_back({source, path});
            else
                pending_sources_.insert(source);
        }
    }
    schedule_flush();
}

void SourceWatcher::schedule_flush() {
    if (stopped_ || flushing_ || debounce_timer_ != 0 ||
        (pending_sources_.empty() && candidates_.empty()))
        return;
    debounce_timer_ = loop_.add_timer(150ms, 0ms, [this] {
        debounce_timer_ = 0;
        flush();
    });
}

void SourceWatcher::flush() {
    if (stopped_ || flushing_)
        return;
    auto sources = std::move(pending_sources_);
    pending_sources_.clear();
    auto candidates = std::move(candidates_);
    candidates_.clear();
    for (auto it = sources.begin(); it != sources.end();) {
        if (!roots_.contains(*it))
            it = sources.erase(it);
        else
            ++it;
    }
    flushing_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::prefetch,
        [client = client_, sources = std::move(sources),
         candidates = std::move(candidates)](const core::CancelToken& cancel) mutable {
            for (const auto& candidate : candidates) {
                if (cancel.cancelled())
                    return Json{{"cancelled", true}};
                std::error_code error;
                if (std::filesystem::is_directory(candidate.path, error))
                    sources.insert(candidate.source);
            }
            Json result{{"ok", true}, {"queued", false}, {"errors", Json::array()}};
            for (Id source : sources) {
                if (cancel.cancelled())
                    return Json{{"cancelled", true}};
                const auto response = client->request(
                    {{"op", "refresh"}, {"source_id", source}, {"reconcile", true}}, &cancel);
                if (!response.value("ok", false))
                    result["errors"].push_back(
                        response.value("error", std::string("Cannot refresh photo source")));
                else if (!response.value("job_ids", Json::array()).empty())
                    result["queued"] = true;
            }
            return result;
        },
        [this](Json response) {
            flushing_ = false;
            schedule_flush();
            if (response.value("cancelled", false))
                return;
            std::string error;
            for (const auto& message : response.value("errors", Json::array())) {
                if (!error.empty())
                    error += '\n';
                error += message.get<std::string>();
            }
            if (changed_ && (response.value("queued", false) || !error.empty()))
                changed_(error);
        });
}
}  // namespace rummage::photos
