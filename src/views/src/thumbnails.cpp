#include "rummage/views/thumbnails.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include "rummage/core/log.hpp"
#include "rummage/fs/entry.hpp"

namespace rummage::views {
namespace {

// Give a busy helper pool time to recover without endlessly retrying a missing helper.
constexpr std::array kRetryDelays{std::chrono::milliseconds{250}, std::chrono::milliseconds{1000}};

std::string slot_key(const std::filesystem::path& file, std::int64_t modified_ns, thumbs::Size size,
                     std::string_view mime, ThumbnailCache mode) {
    std::string key = file.string();
    key += '\n';
    key += thumbs::directory_name(size);
    key += '\n';
    key += std::to_string(modified_ns);
    key += mime;
    key += mode == ThumbnailCache::offline    ? "\noffline"
           : mode == ThumbnailCache::snapshot ? "\nsnapshot"
                                              : "\ncurrent";
    return key;
}

}  // namespace

ThumbnailOptions ThumbnailOptions::for_entry(const fs::Entry& entry, bool inspector) {
    return {.cache = !entry.can_preview() ? ThumbnailCache::offline
                     : entry.preview_current_location || !entry.modified_known
                         ? ThumbnailCache::current
                         : ThumbnailCache::snapshot,
            .inspector = inspector};
}

Thumbnails::Thumbnails(core::EventLoop& loop, core::WorkerPool& pool,
                       std::shared_ptr<const thumbs::Cache> cache, std::function<void()> redraw)
    : loop_{loop}, pool_{pool}, cache_{std::move(cache)}, redraw_{std::move(redraw)} {}

Thumbnails::~Thumbnails() {
    *alive_ = false;
    for (auto& [key, slot] : slots_) {
        if (slot.job) {
            slot.job->cancel();
        }
        if (slot.retry) {
            loop_.remove(*slot.retry);
        }
    }
}

const ui::Texture* Thumbnails::get(const std::filesystem::path& file, std::int64_t modified_ns,
                                   thumbs::Size size, std::string_view mime,
                                   picture::Failure* failure, ThumbnailOptions options,
                                   thumbs::Size* delivered_tier) {
    return get_shared(file, modified_ns, size, mime, failure, options, delivered_tier).get();
}

std::shared_ptr<const ui::Texture> Thumbnails::get_shared(const std::filesystem::path& file,
                                                          std::int64_t modified_ns,
                                                          thumbs::Size size, std::string_view mime,
                                                          picture::Failure* failure,
                                                          ThumbnailOptions options,
                                                          thumbs::Size* delivered_tier) {
    const std::string key = slot_key(file, modified_ns, size, mime, options.cache);
    if (delivered_tier != nullptr) {
        *delivered_tier = size;
    }
    auto [it, added] = slots_.try_emplace(key);
    Slot& slot = it->second;
    slot.used = frame_;
    slot.warming = false;
    bool promoted = false;
    if (options.inspector && !slot.inspector && slot.job) {
        // Claim queued work before cancelling: a worker that already started gets to finish,
        // and a cancelled old delivery can never overwrite the replacement slot.
        auto queued = JobState::queued;
        if (slot.state->compare_exchange_strong(queued, JobState::cancelled)) {
            slot.job->cancel();
            slot.job.reset();
            --in_flight_;
            slot.attempts = 0;
            promoted = true;
        }
    }
    slot.inspector = slot.inspector || options.inspector;
    if (added || promoted) {
        request(key, file, size, std::string{mime}, modified_ns, options.cache);
    }
    if (failure != nullptr) {
        *failure = slot.failed ? slot.failure : picture::Failure::none;
    }
    const auto upload = [this](Slot& ready) {
        if (ready.pixels) {
            ready.texture = std::make_shared<ui::Texture>(ready.pixels->width, ready.pixels->height,
                                                          ready.pixels->rgba);
            bytes_ += ready.texture->bytes();
            ready.pixels.reset();
        }
    };
    upload(slot);
    if (!slot.texture && options.inspector) {
        // A smaller preview of this exact snapshot is useful while the full tier loads.
        // Never substitute a previous selection or trigger another lower-resolution read.
        for (const auto tier : {thumbs::Size::x_large, thumbs::Size::large, thumbs::Size::normal}) {
            if (thumbs::pixels(tier) >= thumbs::pixels(size)) {
                continue;
            }
            const auto found = slots_.find(slot_key(file, modified_ns, tier, mime, options.cache));
            if (found == slots_.end() || (!found->second.texture && !found->second.pixels)) {
                continue;
            }
            auto& fallback = found->second;
            fallback.used = frame_;
            fallback.warming = false;
            upload(fallback);
            if (delivered_tier != nullptr) {
                *delivered_tier = tier;
            }
            return fallback.texture;
        }
    }
    return slot.texture;
}

void Thumbnails::request(const std::string& key, const std::filesystem::path& file,
                         thumbs::Size size, const std::string& mime, std::int64_t modified_ns,
                         ThumbnailCache mode) {
    Slot& slot = slots_.at(key);
    slot.failed = false;
    slot.failure = picture::Failure::none;
    ++slot.attempts;
    dispatch(key, file, size, mime, modified_ns, mode, mode != ThumbnailCache::current);
}

void Thumbnails::dispatch(const std::string& key, const std::filesystem::path& file,
                          thumbs::Size size, const std::string& mime, std::int64_t modified_ns,
                          ThumbnailCache mode, bool cache_probe) {
    Slot& slot = slots_.at(key);
    slot.cache_probe = cache_probe;
    ++in_flight_;
    slot.state = std::make_shared<std::atomic<JobState>>(JobState::queued);
    // Local cache hits must not queue behind slow original reads, including those occupying
    // both reserved preview workers. This lane only reads/decodes local thumbnail PNGs.
    auto& workers = cache_probe      ? pool_.thumbnail_cache_lane()
                    : slot.inspector ? pool_.preview_lane()
                                     : pool_;
    slot.job = workers.submit(
        cache_probe && !slot.inspector ? core::Priority::nearby : core::Priority::visible,
        [cache = cache_, file, size, mime = std::string{mime}, modified_ns, cache_probe,
         state = slot.state](const core::CancelToken& token) {
            auto queued = JobState::queued;
            if (!state->compare_exchange_strong(queued, JobState::running)) {
                return std::make_shared<thumbs::Thumbnail>();
            }
            thumbs::Thumbnail thumbnail =
                cache_probe ? cache->cached(file, size, modified_ns / 1'000'000'000)
                            : cache->get(file, size, mime, &token);
            return std::make_shared<thumbs::Thumbnail>(std::move(thumbnail));
        },
        [this, alive = alive_, key, file, size, mime, modified_ns, mode,
         cache_probe](const std::shared_ptr<thumbs::Thumbnail>& made) {
            if (!*alive) {
                return;
            }
            const auto found = slots_.find(key);
            if (found == slots_.end()) {
                return;  // dropped meanwhile
            }
            --in_flight_;
            found->second.job.reset();
            if (cache_probe && made->image.empty() && mode == ThumbnailCache::snapshot) {
                // A cache miss may touch the original, so dispatch it to a source worker.
                // The probe and source read are one attempt for retry/backoff accounting.
                dispatch(key, file, size, mime, modified_ns, mode, false);
                return;
            }
            if (found->second.warming) {
                // Cache::get has stored a successful result on disk. Offscreen decodes do
                // not accumulate CPU pixels or schedule retries while the user moves on.
                slots_.erase(found);
                redraw_();
                return;
            }
            if (made->image.empty()) {
                found->second.failed = true;
                found->second.failure = made->failure;
                log::debug("thumbnail: {} ({}, attempt {}): {}", file.string(),
                           thumbs::directory_name(size), found->second.attempts, made->error);
                if (made->failure == picture::Failure::unavailable &&
                    found->second.attempts <= kRetryDelays.size()) {
                    ++in_flight_;
                    found->second.retry = loop_.add_timer(
                        kRetryDelays.at(found->second.attempts - 1), std::chrono::milliseconds{0},
                        [this, key, file, size, mime, modified_ns, mode] {
                            slots_.at(key).retry.reset();
                            --in_flight_;
                            request(key, file, size, mime, modified_ns, mode);
                            redraw_();
                        });
                }
            } else {
                found->second.pixels = std::make_shared<picture::Image>(std::move(made->image));
            }
            redraw_();
        });
}

void Thumbnails::end_frame() {
    // Keep only a small fixed set of already-started reads alive offscreen. Repeatedly
    // cancelling SSHFS reads before completion otherwise never warms the disk cache.
    constexpr std::size_t kInspectorWarmJobs = 2;
    constexpr std::size_t kGridWarmJobs = 4;
    std::size_t inspector_warming = 0;
    std::size_t grid_warming = 0;
    for (const auto& [key, slot] : slots_) {
        if (slot.warming) {
            ++(slot.inspector ? inspector_warming : grid_warming);
        }
    }
    // Cancel unseen queued work. Temporary failures get a fresh attempt on return,
    // including those that exhausted their automatic retries while visible.
    for (auto it = slots_.begin(); it != slots_.end();) {
        Slot& slot = it->second;
        if (slot.used != frame_ &&
            (slot.job || slot.pixels || slot.failure == picture::Failure::unavailable)) {
            if (slot.job) {
                auto& warming = slot.inspector ? inspector_warming : grid_warming;
                const auto limit = slot.inspector ? kInspectorWarmJobs : kGridWarmJobs;
                auto queued = JobState::queued;
                const bool was_queued =
                    slot.state->compare_exchange_strong(queued, JobState::cancelled);
                if (!slot.cache_probe && !was_queued && (slot.warming || warming < limit)) {
                    if (!slot.warming) {
                        slot.warming = true;
                        ++warming;
                    }
                    ++it;
                    continue;
                }
                slot.job->cancel();
                --in_flight_;
            }
            if (slot.retry) {
                loop_.remove(*slot.retry);
                --in_flight_;
            }
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    // Over budget: drop the textures drawn longest ago (never this frame's or a pinned visual).
    // Keep cache ownership of pins so their GL destruction happens here, not in a transfer
    // completion callback that releases its saved visual outside a draw.
    if (bytes_ > kBudgetBytes) {
        std::vector<std::pair<std::uint64_t, std::string>> by_age;
        for (const auto& [key, slot] : slots_) {
            if (slot.texture && slot.texture.use_count() == 1 && slot.used != frame_) {
                by_age.emplace_back(slot.used, key);
            }
        }
        std::ranges::sort(by_age);
        for (const auto& [used, key] : by_age) {
            if (bytes_ <= kBudgetBytes) {
                break;
            }
            const auto it = slots_.find(key);
            if (it != slots_.end() && it->second.texture) {
                bytes_ -= it->second.texture->bytes();
                slots_.erase(it);
            }
        }
    }
    ++frame_;
}

Thumbnails::Usage Thumbnails::usage() const {
    Usage usage{.slots = slots_.size(), .textures = 0, .texture_bytes = bytes_, .pixel_bytes = 0};
    for (const auto& [key, slot] : slots_) {
        usage.textures += slot.texture ? 1U : 0U;
        usage.pixel_bytes += slot.pixels ? slot.pixels->rgba.size() : 0;
    }
    return usage;
}

}  // namespace rummage::views
