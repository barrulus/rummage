#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "rummage/core/worker_pool.hpp"
#include "rummage/picture/image.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/texture.hpp"

namespace rummage::fs {
struct Entry;
}

namespace rummage::views {

// Current-location previews must validate against the file now; normal listings can first
// reuse the snapshot they already display. Offline browsing never opens the original.
enum class ThumbnailCache : unsigned char { current, snapshot, offline };

struct ThumbnailOptions {
    ThumbnailCache cache{ThumbnailCache::current};
    bool inspector{false};

    [[nodiscard]] static ThumbnailOptions for_entry(const fs::Entry& entry, bool inspector = false);
};

// Thumbnails for the views, as GPU textures. A view asks for the files it draws, every frame;
// the ones not ready are made on workers through the freedesktop cache. Inspector requests
// have their own worker lane. Unseen queued work is cancelled; a bounded number of started
// jobs may finish warming the disk cache. Textures beyond the budget are dropped oldest first.
//
// Loop thread only. Textures are created inside get(), so call it while drawing (GL current).
class Thumbnails {
public:
    static constexpr std::size_t kBudgetBytes = std::size_t{256} << 20;

    Thumbnails(core::EventLoop& loop, core::WorkerPool& pool,
               std::shared_ptr<const thumbs::Cache> cache, std::function<void()> redraw);
    ~Thumbnails();

    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;
    Thumbnails(Thumbnails&&) = delete;
    Thumbnails& operator=(Thumbnails&&) = delete;

    // The thumbnail of file (absolute) as it was at modified_ns, at size; nullptr while it is
    // being made (now asked for) or when it cannot be made. When it cannot, failure (if given)
    // says why, when the file says so (damaged, password protected); else none.
    // Inspector requests reserve decoding capacity and may show an existing smaller preview
    // of the same snapshot while the requested tier loads. delivered_tier reports that tier
    // when non-null (the requested tier while blank).
    [[nodiscard]] const ui::Texture* get(const std::filesystem::path& file,
                                         std::int64_t modified_ns, thumbs::Size size,
                                         std::string_view mime = {},
                                         picture::Failure* failure = nullptr,
                                         ThumbnailOptions options = {},
                                         thumbs::Size* delivered_tier = nullptr);

    // Pin the already displayed texture for an operation; cache eviction waits for release.
    [[nodiscard]] std::shared_ptr<const ui::Texture> get_shared(
        const std::filesystem::path& file, std::int64_t modified_ns, thumbs::Size size,
        std::string_view mime = {}, picture::Failure* failure = nullptr,
        ThumbnailOptions options = {}, thumbs::Size* delivered_tier = nullptr);

    // After each frame: cancels unseen work except bounded cache warming, keeps to the budget.
    void end_frame();

    // Thumbnails are being made or waiting for a bounded retry (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept { return in_flight_ > 0; }

    [[nodiscard]] std::size_t texture_bytes() const noexcept { return bytes_; }

    // What it holds (the memory report).
    struct Usage {
        std::size_t slots{0};
        std::size_t textures{0};
        std::size_t texture_bytes{0};
        std::size_t pixel_bytes{0};  // made, not uploaded yet
    };

    [[nodiscard]] Usage usage() const;

private:
    enum class JobState : unsigned char { queued, running, cancelled };

    struct Slot {
        std::optional<core::CancelToken> job;  // being made
        std::optional<core::EventLoop::SourceId> retry;
        unsigned attempts{0};
        std::shared_ptr<std::atomic<JobState>> state;
        bool inspector{false};
        bool warming{false};
        bool cache_probe{false};  // Local PNG reads never occupy source/preview workers.
        std::shared_ptr<picture::Image> pixels;  // made, not uploaded yet
        std::shared_ptr<ui::Texture> texture;
        bool failed{false};
        picture::Failure failure{picture::Failure::none};  // why, when failed
        std::uint64_t used{0};                             // the frame that last asked for it
    };

    void request(const std::string& key, const std::filesystem::path& file, thumbs::Size size,
                 const std::string& mime, std::int64_t modified_ns, ThumbnailCache mode);
    void dispatch(const std::string& key, const std::filesystem::path& file, thumbs::Size size,
                  const std::string& mime, std::int64_t modified_ns, ThumbnailCache mode,
                  bool cache_probe);

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    std::shared_ptr<const thumbs::Cache> cache_;
    std::function<void()> redraw_;
    std::unordered_map<std::string, Slot> slots_;  // by file, size and mtime
    std::uint64_t frame_{1};
    std::size_t bytes_{0};
    std::size_t in_flight_{0};
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
