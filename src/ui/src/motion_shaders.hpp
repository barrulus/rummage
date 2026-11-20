#pragma once

#include <string>

#include "rummage/core/motion.hpp"

namespace rummage::ui {

// Transition built-ins. GLES 3, sRGB-encoded premultiplied RGBA. Local UVs run top to bottom;
// framebuffer copies run bottom to top and are flipped only in the sampling helpers.
inline constexpr const char* kMotionVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_rect;
uniform vec2 u_viewport;
out vec2 uv;
void main() {
    uv = a_corner;
    vec2 p = (a_rect.xy + a_corner * a_rect.zw) / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kMotionFragmentBody = R"glsl(

in vec2 uv;
out vec4 color;
uniform sampler2D source_texture;
uniform sampler2D target_texture;
uniform float progress;
uniform float time_seconds;
uniform vec2 resolution;
uniform vec2 capture_extent;
uniform vec2 origin;
uniform int effect;
uniform vec4 accent;
uniform float hologram_spread;
uniform float portal_rotation;

vec2 sample_uv(vec2 p) {
    p = clamp(p, 0.5 / resolution, 1.0 - 0.5 / resolution);
    return vec2(p.x, 1.0 - p.y) * capture_extent;
}
vec4 source(vec2 p) { return texture(source_texture, sample_uv(p)); }
vec4 target(vec2 p) { return texture(target_texture, sample_uv(p)); }
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1,0)), f.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
}
void main() {
    float p = clamp(progress, 0.0, 1.0);
    vec4 old = source(uv), fresh = target(uv);
    // Exact endpoints avoid a final-frame seam, including translucent windows.
    if (p >= 1.0) { color = effect == motion_disintegrate ? old : fresh; return; }
    float eased = 1.0 - pow(1.0 - p, 3.0);
    float pulse = sin(p * 3.14159265);
    vec2 local = uv * resolution;
    vec2 delta = local - origin;
    float radius = max(length(max(origin, resolution - origin)), 1.0);
    float distance = length(delta) / radius;
    float mask = eased;
    if (effect == motion_slide || effect == motion_scale) {
        vec2 q = effect == motion_slide ? uv - vec2(0, (1.0 - eased) * 0.06)
                            : (uv - 0.5) / mix(0.94, 1.0, eased) + 0.5;
        fresh = target(clamp(q, 0.0, 1.0));
        mask *= step(0.0, q.x) * step(q.x, 1.0) * step(0.0, q.y) * step(q.y, 1.0);
    } else if (effect == motion_iris) {
        mask = 1.0 - smoothstep(eased - 0.02, eased + 0.02, distance);
    } else if (effect == motion_ripple) {
        float wave = sin(distance * 28.0 - p * 12.0) * pulse * 3.0;
        fresh = target(clamp(uv + normalize(delta + vec2(0.001)) * wave / resolution, 0.0, 1.0));
        mask = 1.0;
    } else if (effect == motion_ink_bleed || effect == motion_particles) {
        float n = noise(local / (effect == motion_particles ? 5.0 : 36.0));
        mask = smoothstep(n - 0.08, n + 0.08, p * 1.16 - 0.08);
        if (effect == motion_particles) {
            float dust = pow(hash(floor(local / 4.0)), 24.0) * pulse * (1.0 - mask);
            old = mix(old, accent, dust * 0.5);
        }
    } else if (effect == motion_disintegrate) {
        // This is an overlay sprite: restore the captured background as it erodes.
        mask = 1.0 - smoothstep(noise(local / 4.0) - 0.08,
                                noise(local / 4.0) + 0.08, p * 1.16 - 0.08);
        float dust = pow(hash(floor((local + vec2(p * 20.0, p * 12.0)) / 3.0)), 18.0) * pulse;
        fresh = mix(fresh, accent, dust * 0.6);
    } else if (effect == motion_hologram || effect == motion_glitch) {
        float shift = (effect == motion_hologram ? hologram_spread : 3.0) * pulse / resolution.x;
        vec2 q = uv;
        if (effect == motion_glitch) q.x += sin(floor(local.y / 8.0) + time_seconds * 70.0) * shift;
        fresh = target(clamp(q, 0.0, 1.0));
        vec4 red = target(clamp(q + vec2(shift,0), 0.0, 1.0));
        vec4 blue = target(clamp(q - vec2(shift,0), 0.0, 1.0));
        fresh.a = max(fresh.a, max(red.a, blue.a));
        fresh.rgb = min(vec3(red.r, fresh.g, blue.b), vec3(fresh.a));
        mask = effect == motion_hologram ? 0.75 : 1.0;
    } else if (effect == motion_portal) {
        float phase = abs(p * 2.0 - 1.0);
        float size = max(0.05, phase);
        vec2 q = (uv - 0.5) / size;
        float angle = (1.0 - phase) * portal_rotation;
        q = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * q + 0.5;
        fresh = target(clamp(q, 0.0, 1.0));
        mask = step(0.0,q.x) * step(q.x,1.0) * step(0.0,q.y) * step(q.y,1.0) * phase;
    } else if (effect == motion_energy_trace || effect == motion_focus_glow) {
        float edge = min(min(local.x, resolution.x - local.x), min(local.y, resolution.y - local.y));
        float glow = effect == motion_energy_trace ? exp(-edge / 2.0) : exp(-dot(delta, delta) / 2200.0);
        fresh = mix(fresh, accent, glow * pulse * 0.22);
        mask = 1.0;
    } else if (effect == motion_frost) {
        vec2 d = vec2(3.0 * pulse) / resolution;
        old = (source(uv) * 4.0 + source(uv + vec2(d.x,0)) + source(uv - vec2(d.x,0))
             + source(uv + vec2(0,d.y)) + source(uv - vec2(0,d.y))) / 8.0;
    } else if (effect == motion_scan) {
        mask = 1.0 - smoothstep(p - 0.02, p + 0.02, uv.y);
        fresh = mix(fresh, accent, exp(-abs(uv.y - p) * resolution.y / 2.0) * 0.3);
    }
    color = mix(old, fresh, clamp(mask, 0.0, 1.0));
}
)glsl";

// Persistent shaders share a small, versioned authoring interface. Colours returned by
// effect() are straight RGBA; this wrapper clamps and premultiplies them for the renderer.
inline std::string ambient_fragment_shader(std::string_view body) {
    return std::string{R"glsl(#version 300 es
precision highp float;
in vec2 uv;
out vec4 color;
uniform float time_seconds;
uniform vec2 resolution;
uniform vec4 accent;
uniform float intensity;
)glsl"} + std::string{body} +
           R"glsl(
void main() {
    vec4 c = clamp(effect(uv), 0.0, 1.0);
    c.a *= intensity;
    color = vec4(c.rgb * c.a, c.a);
}
)glsl";
}

inline constexpr std::string_view kAmbientBackground = R"glsl(
vec4 effect(vec2 p) {
    float wave = sin(p.x * 6.283185 + sin(p.y * 4.0 + time_seconds * 0.3)
                     + time_seconds * 0.2);
    float veil = 0.5 + 0.5 * sin(p.y * 5.0 - time_seconds * 0.25 + wave);
    return vec4(accent.rgb, (0.25 + 0.75 * veil) * (0.6 + 0.4 * wave));
}
)glsl";

inline constexpr std::string_view kAmbientSelection = R"glsl(
vec4 effect(vec2 p) {
    vec2 edge = min(p, 1.0 - p) * resolution;
    float distance = min(edge.x, edge.y);
    float outline = 1.0 - smoothstep(1.0, 4.0, distance);
    float breath = 0.65 + 0.35 * sin(time_seconds * 2.0);
    return vec4(accent.rgb, outline * breath);
}
)glsl";

// Generated once when the lazy pipeline is built. No shader-side enum numbers to maintain.
inline std::string motion_fragment_shader() {
    std::string shader = "#version 300 es\nprecision highp float;\n";
    for (const auto& effect : core::kMotionEffectRegistry) {
        shader += "const int motion_";
        shader += effect.name;
        shader += " = " + std::to_string(static_cast<int>(effect.id)) + ";\n";
    }
    shader += kMotionFragmentBody;
    return shader;
}

}  // namespace rummage::ui
