#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rummage/picture/image.hpp"

namespace rummage::isolate {
// A playback session owns a dedicated decoder and raw-PCM audio sink. Only bounded decoded
// data crosses into this process. Methods run on the UI thread; I/O runs on a private thread.
class Playback {
public:
    struct Snapshot {
        std::shared_ptr<const picture::Image> image;
        std::int64_t position_ms{0}, duration_ms{0};
        bool loading{true}, paused{true}, ended{false}, failed{false};
        std::string error;
    };

    struct Programs {
        std::filesystem::path decoder, audio;
    };

    explicit Playback(std::filesystem::path path, Programs programs = {});
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    void pause(bool value);
    void seek(std::int64_t milliseconds);
    void volume(int percent);
    [[nodiscard]] Snapshot snapshot() const;

    [[nodiscard]] int volume() const { return volume_.load(); }

private:
    void run(std::stop_token stop, const std::filesystem::path& path, const Programs& programs);
    mutable std::mutex mutex_;
    Snapshot state_;
    std::atomic<bool> paused_{true};
    std::atomic<std::int64_t> seek_{-1};
    std::atomic<int> volume_{80};
    std::jthread thread_;
};
}  // namespace rummage::isolate
