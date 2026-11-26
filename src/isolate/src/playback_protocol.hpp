#pragma once
#include <cstdint>

namespace rummage::isolate::playback_wire {
// Private versioned stream protocol, separate from still-preview protocol 8. Every response
// carries exactly one sealed memfd: width*height RGBA bytes followed by stereo s16le PCM.
inline constexpr std::uint32_t kMagic = 0x52504c31;  // RPL1
inline constexpr std::uint32_t kMaxSide = 1280;
inline constexpr std::uint32_t kMaxSamples = 48000 * 2;  // one second of stereo, hard cap
inline constexpr std::uint32_t kStepMs = 50;

struct Request {
    std::uint32_t magic{kMagic};
    std::uint32_t seek{0};
    std::int64_t position_ms{0};
};

struct Response {
    std::uint32_t magic{kMagic};
    std::uint32_t status{0};  // 0 okay, 1 EOF, 2 unsupported/damaged, 3 seek failed
    std::int64_t duration_ms{0};
    std::uint32_t width{0}, height{0}, samples{0}, audio{0};
};
}  // namespace rummage::isolate::playback_wire
