#include "rummage/views/thumbnails.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include "rummage/core/log.hpp"

namespace rummage::views {
namespace {

// Give a busy helper pool time to recover without endlessly retrying a missing helper.
constexpr std::array kRetryDelays{std::chrono::milliseconds{250}, std::chrono::milliseconds{1000}};

}  // namespace

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
                                   picture::Failure* failure, bool offline) {
    return get_shared(file, modified_ns, size, mime, failure, offline).get();
}

std::shared_ptr<const ui::Texture> Thumbnails::get_shared(const std::filesystem::path& file,
                                                          std::int64_t modified_ns,
                                                          thumbs::Size size, std::string_view mime,
                                                          picture::Failure* failure, bool offline) {
    std::string key = file.string();
    key += '\n';
    key += thumbs::directory_name(size);
    key += '\n';
    key += std::to_string(modified_ns);
    key += mime;
    key += offline ? "\noffline" : "\nonline";
    auto [it, added] = slots_.try_emplace(key);
    Slot& slot = it->second;
    slot.used = frame_;
    if (added) {
        request(key, file, size, std::string{mime}, modified_ns, offline);
    }
    if (failure != nullptr) {
        *failure = slot.failed ? slot.failure : picture::Failure::none;
    }
    if (slot.pixels) {
        slot.texture = std::make_shared<ui::Texture>(slot.pixels->width, slot.pixels->height,
                                                     slot.pixels->rgba);
        bytes_ += slot.texture->bytes();
        slot.pixels.reset();
    }
    return slot.texture;
}

void Thumbnails::request(const std::string& key, const std::filesystem::path& file,
                         thumbs::Size size, const std::string& mime, std::int64_t modified_ns,
                         bool offline) {
    Slot& slot = slots_.at(key);
    slot.failed = false;
    slot.failure = picture::Failure::none;
    ++slot.attempts;
    ++in_flight_;
    slot.job = pool_.submit(
        core::Priority::visible,
        [cache = cache_, file, size, mime = std::string{mime}, modified_ns,
         offline](const core::CancelToken& token) {
            thumbs::Thumbnail thumbnail =
                offline ? cache->cached(file, size, modified_ns / 1'000'000'000)
                        : cache->get(file, size, mime, &token);
            return std::make_shared<thumbs::Thumbnail>(std::move(thumbnail));
        },
        [this, alive = alive_, key, file, size, mime, modified_ns,
         offline](const std::shared_ptr<thumbs::Thumbnail>& made) {
            if (!*alive) {
                return;
            }
            const auto found = slots_.find(key);
            if (found == slots_.end()) {
                return;  // dropped meanwhile
            }
            --in_flight_;
            found->second.job.reset();
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
                        [this, key, file, size, mime, modified_ns, offline] {
                            slots_.at(key).retry.reset();
                            --in_flight_;
                            request(key, file, size, mime, modified_ns, offline);
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
    // Cancel unseen work. Temporary failures must also get a fresh attempt on return,
    // including those that exhausted their automatic retries while visible.
    for (auto it = slots_.begin(); it != slots_.end();) {
        Slot& slot = it->second;
        if (slot.used != frame_ &&
            (slot.job || slot.pixels || slot.failure == picture::Failure::unavailable)) {
            if (slot.job) {
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
