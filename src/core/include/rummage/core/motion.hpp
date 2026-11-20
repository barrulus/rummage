#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rummage::core {

enum class MotionEvent : std::uint8_t {
    directory_open,
    directory_back,
    view_change,
    file_copy,
    file_move,
    file_trash,
    selection_change,
    focus_change,
    invalid_operation,
    quick_look,
    palette_open,
    search_results,
    count
};

enum class MotionEffect : std::uint8_t {
    none,
    fade,
    slide,
    scale,
    iris,
    ripple,
    ink_bleed,
    disintegrate,
    hologram,
    portal,
    energy_trace,
    focus_glow,
    glitch,
    frost,
    particles,
    scan,
    count
};

inline constexpr std::array<std::string_view, 12> kMotionEvents{
    "directory_open",    "directory_back", "view_change",      "file_copy",
    "file_move",         "file_trash",     "selection_change", "focus_change",
    "invalid_operation", "quick_look",     "palette_open",     "search_results"};

struct MotionEffectInfo {
    MotionEffect id;
    std::string_view name;  // config name and generated GLSL constant suffix
    bool native_feedback{false};
};

// Configuration, rendering dispatch and GLSL identifiers use this same registry.
inline constexpr std::array kMotionEffectRegistry{
    MotionEffectInfo{MotionEffect::none, "none"},
    MotionEffectInfo{MotionEffect::fade, "fade"},
    MotionEffectInfo{MotionEffect::slide, "slide"},
    MotionEffectInfo{MotionEffect::scale, "scale"},
    MotionEffectInfo{MotionEffect::iris, "iris"},
    MotionEffectInfo{MotionEffect::ripple, "ripple"},
    MotionEffectInfo{MotionEffect::ink_bleed, "ink_bleed"},
    MotionEffectInfo{MotionEffect::disintegrate, "disintegrate"},
    MotionEffectInfo{MotionEffect::hologram, "hologram"},
    MotionEffectInfo{MotionEffect::portal, "portal"},
    MotionEffectInfo{MotionEffect::energy_trace, "energy_trace", true},
    MotionEffectInfo{MotionEffect::focus_glow, "focus_glow", true},
    MotionEffectInfo{MotionEffect::glitch, "glitch"},
    MotionEffectInfo{MotionEffect::frost, "frost"},
    MotionEffectInfo{MotionEffect::particles, "particles"},
    MotionEffectInfo{MotionEffect::scan, "scan"}};

static_assert(kMotionEffectRegistry.size() == static_cast<std::size_t>(MotionEffect::count));
static_assert(
    [] {
        for (std::size_t i = 0; i < kMotionEffectRegistry.size(); ++i) {
            if (static_cast<std::size_t>(kMotionEffectRegistry[i].id) != i)
                return false;
            for (std::size_t j = 0; j < i; ++j)
                if (kMotionEffectRegistry[i].name == kMotionEffectRegistry[j].name)
                    return false;
        }
        return true;
    }(),
    "Every effect must have one unique registry entry in enum order");

inline constexpr auto kMotionEffects = [] {
    std::array<std::string_view, kMotionEffectRegistry.size()> names{};
    for (const auto& effect : kMotionEffectRegistry)
        names[static_cast<std::size_t>(effect.id)] = effect.name;
    return names;
}();

[[nodiscard]] constexpr bool native_motion_feedback(MotionEffect effect) noexcept {
    const auto index = static_cast<std::size_t>(effect);
    return index < kMotionEffectRegistry.size() && kMotionEffectRegistry[index].native_feedback;
}

template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::optional<Enum> motion_name(
    std::string_view name, const std::array<std::string_view, N>& names) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == name)
            return static_cast<Enum>(i);
    return std::nullopt;
}

struct MotionConfig {
    bool enabled{true};
    float duration_scale{1.0F};
    enum class Reduce { system, always, never };
    Reduce reduce_motion{Reduce::system};
    enum class Quality { automatic, low, high };
    Quality quality{Quality::automatic};
    float hologram_spread{2.0F};  // physical pixels
    float portal_rotation{2.5F};  // radians at maximum contraction

    struct Ambient {
        bool background{false};
        bool selection_glow{false};
        float speed{1.0F};
        float intensity{0.15F};
        int fps{30};
        std::string shader;  // optional absolute path; empty selects the built-in background
        bool operator==(const Ambient&) const = default;
    } ambient{};

    std::array<MotionEffect, kMotionEvents.size()> events{
        MotionEffect::iris,         MotionEffect::ripple,     MotionEffect::ink_bleed,
        MotionEffect::hologram,     MotionEffect::portal,     MotionEffect::disintegrate,
        MotionEffect::energy_trace, MotionEffect::focus_glow, MotionEffect::glitch,
        MotionEffect::frost,        MotionEffect::particles,  MotionEffect::scan};
    std::array<int, kMotionEvents.size()> durations_ms{220, 180, 240, 420, 420, 420,
                                                       160, 140, 140, 220, 240, 180};
    bool operator==(const MotionConfig&) const = default;
};

}  // namespace rummage::core
