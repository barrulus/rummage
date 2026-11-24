#include "rummage/ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace rummage::ui {

float Motion::ease(float progress) noexcept {
    const float p = std::clamp(progress, 0.0F, 1.0F);
    return 1.0F - (1.0F - p) * (1.0F - p) * (1.0F - p);
}

Rect Motion::Frame::geometry() const noexcept {
    // The portal closes on the source and opens at the destination. Its shader is
    // fully transparent at the handoff, so no travelling/shrunken icon jumps panes.
    if (effect == core::MotionEffect::portal)
        return progress < 0.5F ? from : to;
    return {std::lerp(from.x, to.x, eased), std::lerp(from.y, to.y, eased),
            std::lerp(from.width, to.width, eased), std::lerp(from.height, to.height, eased)};
}

void Motion::configure(const core::MotionConfig& config) {
    if (config_ != config) {
        config_ = config;
        cancel_all();
    }
}

void Motion::set_system_reduced(bool reduced) {
    if (system_reduced_ != reduced) {
        system_reduced_ = reduced;
        cancel_all();
    }
}

bool Motion::reduced() const noexcept {
    return config_.reduce_motion == core::MotionConfig::Reduce::always ||
           (config_.reduce_motion == core::MotionConfig::Reduce::system && system_reduced_);
}

bool Motion::ambient_enabled() const noexcept {
    return config_.enabled && !reduced() && config_.duration_scale > 0 &&
           config_.ambient.intensity > 0 &&
           (config_.ambient.background || config_.ambient.selection_glow);
}

bool Motion::start(MotionChannel channel, core::MotionEvent event, Clock::time_point now, Rect from,
                   Rect to) {
    const auto index = static_cast<std::size_t>(event);
    const auto slot = static_cast<std::size_t>(channel);
    if (slot >= running_.size() || index >= config_.events.size())
        return false;
    cancel(channel);
    // Reduced motion is deliberately immediate: no flashes, travel, masks or blur.
    if (!config_.enabled || reduced() || !std::isfinite(config_.duration_scale) ||
        config_.duration_scale <= 0 || config_.events[index] == core::MotionEffect::none ||
        config_.durations_ms[index] <= 0)
        return false;
    auto effect = config_.events[index];
    if (config_.quality == core::MotionConfig::Quality::low)
        effect = core::MotionEffect::fade;
    const float seconds = std::clamp(
        static_cast<float>(config_.durations_ms[index]) * config_.duration_scale / 1000.0F, 0.001F,
        2.0F);
    running_[slot] = Running{.frame = {.effect = effect,
                                       .from = from,
                                       .to = to,
                                       .hologram_spread = config_.hologram_spread,
                                       .portal_rotation = config_.portal_rotation},
                             .start = now,
                             .duration = std::chrono::duration<float>{seconds}};
    return true;
}

void Motion::cancel(MotionChannel channel) {
    const auto slot = static_cast<std::size_t>(channel);
    if (slot < running_.size())
        running_[slot].reset();
}

void Motion::cancel_all() {
    for (auto& running : running_)
        running.reset();
}

void Motion::advance(Clock::time_point now) {
    for (auto& running : running_)
        if (running && now - running->start >= running->duration)
            running.reset();
}

bool Motion::active() const noexcept {
    return std::ranges::any_of(running_, [](const auto& item) { return item.has_value(); });
}

std::optional<Motion::Frame> Motion::frame(MotionChannel channel, Clock::time_point now) const {
    const auto slot = static_cast<std::size_t>(channel);
    if (slot >= running_.size() || !running_[slot])
        return std::nullopt;
    const Running& running = *running_[slot];
    Frame frame = running.frame;
    frame.time_seconds = std::max(0.0F, std::chrono::duration<float>(now - running.start).count());
    frame.progress = std::clamp(frame.time_seconds / running.duration.count(), 0.0F, 1.0F);
    frame.eased = ease(frame.progress);
    return frame;
}

}  // namespace rummage::ui
