#pragma once

#include <array>
#include <chrono>
#include <optional>

#include "rummage/core/motion.hpp"
#include "rummage/ui/geometry.hpp"

namespace rummage::ui {

enum class MotionChannel : std::uint8_t { first_pane, second_pane, overlay, operation, count };

inline constexpr std::array kMotionChannels{MotionChannel::first_pane, MotionChannel::second_pane,
                                            MotionChannel::overlay, MotionChannel::operation};
static_assert(kMotionChannels.size() == static_cast<std::size_t>(MotionChannel::count));

[[nodiscard]] constexpr MotionChannel pane_motion_channel(std::size_t pane) noexcept {
    return pane == 0   ? MotionChannel::first_pane
           : pane == 1 ? MotionChannel::second_pane
                       : MotionChannel::count;
}

// CPU-only, deterministic clock and bounded storage. Channels belong to surfaces, not files.
// Replacing a channel cancels its old effect. It never owns or delays an operation.
class Motion {
public:
    using Clock = std::chrono::steady_clock;

    struct Frame {
        core::MotionEffect effect{core::MotionEffect::none};
        float progress{0};
        float eased{0};
        float time_seconds{0};
        Rect from{};
        Rect to{};
        float hologram_spread{2.0F};
        float portal_rotation{2.5F};
        [[nodiscard]] Rect geometry() const noexcept;
    };

    void configure(const core::MotionConfig& config);
    void set_system_reduced(bool reduced);
    [[nodiscard]] bool reduced() const noexcept;
    [[nodiscard]] bool ambient_enabled() const noexcept;

    [[nodiscard]] const core::MotionConfig& config() const noexcept { return config_; }

    bool start(MotionChannel channel, core::MotionEvent event, Clock::time_point now,
               Rect from = {}, Rect to = {});
    void cancel(MotionChannel channel);
    void cancel_all();
    void advance(Clock::time_point now);
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::optional<Frame> frame(MotionChannel channel, Clock::time_point now) const;
    [[nodiscard]] static float ease(float progress) noexcept;

private:
    struct Running {
        Frame frame;
        Clock::time_point start;
        std::chrono::duration<float> duration;
    };

    core::MotionConfig config_;
    bool system_reduced_{false};
    std::array<std::optional<Running>, kMotionChannels.size()> running_;
};

}  // namespace rummage::ui
