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
uniform bool isolated;
uniform vec2 object_size;

vec2 sample_uv(vec2 p) {
    p = clamp(p, 0.5 / resolution, 1.0 - 0.5 / resolution);
    return vec2(p.x, 1.0 - p.y) * capture_extent;
}
vec4 source(vec2 p) { return texture(source_texture, sample_uv(p)); }
vec4 target(vec2 p) { return texture(target_texture, sample_uv(p)); }
vec4 sprite(vec2 pixel) {
    vec2 q = pixel / resolution;
    if (any(lessThan(q, vec2(0))) || any(greaterThan(q, vec2(1)))) return vec4(0);
    return target(q);
}
vec4 over(vec4 foreground, vec4 background) {
    return foreground + background * (1.0 - foreground.a);
}
vec4 light(vec4 background, vec3 tint, float strength) {
    float alpha = 1.0 - exp(-max(strength, 0.0));
    return over(vec4(tint * alpha, alpha), background);
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1,0)), f.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
}
float turbulence(vec2 p) {
    return noise(p) * 0.57 + noise(p * 2.03 + 17.1) * 0.28
         + noise(p * 4.09 + 31.7) * 0.15;
}

// These effects sample an isolated, premultiplied icon. Padding belongs to the
// effect; the captured directory behind it is never distorted or tinted wholesale.
vec4 object_effect(vec2 local, vec4 backdrop, float p) {
    vec2 d = local - origin;
    float size = max(min(object_size.x, object_size.y), 1.0);
    float envelope = smoothstep(0.0, 0.12, p) * (1.0 - smoothstep(0.82, 1.0, p));
    vec3 cyan = vec3(0.12, 0.64, 1.0);
    vec3 ice = vec3(0.76, 0.95, 1.0);
    if (effect == motion_disintegrate) {
        float front = p * 1.5 - 0.2;
        float field = (d.x / size + 0.5) * 0.72 + turbulence(local / (size * 0.12)) * 0.28;
        float remaining = smoothstep(front - 0.035, front + 0.035, field);
        vec4 icon = sprite(local);
        vec4 result = over(icon * remaining, backdrop);
        float edge = exp(-abs(field - front) * 65.0) * icon.a * envelope;
        result = light(result, vec3(1.0, 0.77, 0.26), edge * 3.5);
        // Stable seeds follow individual trajectories rather than changing noise per frame.
        float sparks = 0.0;
        float haze = 0.0;
        for (int i = 0; i < 96; ++i) {
            vec2 seed = vec2(float(i), 7.0);
            vec2 pos = vec2(hash(seed), hash(seed + 19.0)) - 0.5;
            vec2 start = origin + pos * object_size;
            float birth = ((pos.x + 0.5) * 0.72
                          + turbulence(start / (size * 0.12)) * 0.28 + 0.2) / 1.5;
            float age = max(p - birth, 0.0);
            float live = step(birth, p) * (1.0 - smoothstep(0.12, 0.52, age));
            vec2 velocity = vec2(-0.4 - hash(seed + 41.0),
                                 (hash(seed + 63.0) - 0.6) * 1.5);
            vec2 at = start + size * (velocity * age + vec2(-0.3, -0.4) * age * age);
            vec2 offset = local - at;
            float energy = sprite(start).a * live;
            float r = max(0.55, size * 0.009) * (0.65 + hash(seed + 83.0));
            float dist2 = dot(offset, offset);
            sparks += exp(-dist2 / (r * r)) * energy;
            haze += exp(-dist2 / (r * r * 18.0)) * energy * 0.13;
        }
        result = light(result, vec3(1.0, 0.38, 0.025), haze * envelope);
        return light(result, vec3(1.0, 0.87, 0.42), sparks * 3.0 * envelope);
    }
    if (effect == motion_hologram) {
        float shift = hologram_spread * envelope;
        vec4 icon = sprite(local);
        float ghost = max(sprite(local + vec2(shift, 0)).a,
                          sprite(local - vec2(shift, 0)).a);
        float scan = 0.65 + 0.35 * sin(d.y * 2.4 - p * 24.0);
        float shimmer = turbulence(vec2(d.y * 0.2, p * 7.0));
        float edge = abs(sprite(local + vec2(1,0)).a - sprite(local - vec2(1,0)).a)
                   + abs(sprite(local + vec2(0,1)).a - sprite(local - vec2(0,1)).a);
        vec4 projection = vec4(mix(cyan, ice, clamp(edge, 0.0, 1.0)), 1.0)
                        * icon.a * (0.35 + 0.35 * scan);
        vec4 result = over(mix(icon, projection, envelope), backdrop);
        result = light(result, cyan, ghost * envelope * 0.28);
        result = light(result, ice, edge * envelope * (0.6 + shimmer));
        vec2 base = (d - vec2(0, size * 0.57)) / vec2(size * 0.62, size * 0.15);
        float ring = abs(length(base) - 1.0) * size * 0.15;
        result = light(result, cyan, exp(-ring * 0.55) * envelope * 0.8);
        result = light(result, ice, exp(-ring * 2.3) * envelope * 1.8);
        float beam = exp(-pow(d.x / (size * 0.6), 4.0) * 2.0)
                   * smoothstep(-0.6, 0.5, d.y / size)
                   * (1.0 - smoothstep(0.5, 0.67, d.y / size));
        return light(result, cyan, beam * envelope * 0.14);
    }
    // A dark aperture surrounded by several independently curling plasma filaments.
    float phase = abs(2.0 * p - 1.0);
    envelope *= smoothstep(0.0, 0.18, phase);
    float contraction = mix(0.025, 1.0, smoothstep(0.04, 1.0, phase));
    float angle = (1.0 - phase) * portal_rotation;
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    vec4 icon = sprite(origin + rotation * d / contraction);
    vec4 result = over(icon * smoothstep(0.03, 0.3, phase), backdrop);
    float r = length(d) / size;
    float theta = atan(d.y, d.x);
    float aperture = 0.48 + 0.06 * sin(p * 3.14159265);
    float dark = (1.0 - smoothstep(aperture - 0.12, aperture, r)) * envelope * 0.68;
    result = over(vec4(vec3(0.005, 0.015, 0.035) * dark, dark), result);
    float filaments = 0.0;
    for (int i = 0; i < 4; ++i) {
        float layer = float(i);
        float twist = theta + p * (3.0 + portal_rotation) + layer * 1.7 - r * 5.0;
        float curl = sin(twist * 3.0 + sin(twist * 5.0 + p * 9.0)) * 0.035
                   + sin(twist * 7.0 - p * 13.0) * 0.018;
        float ridge = abs(r - aperture - (layer - 1.5) * 0.025 - curl);
        filaments += exp(-ridge * size * 1.7) * (0.5 + 0.5 * sin(twist * 2.0 + layer));
    }
    float halo = exp(-abs(r - aperture) * 15.0);
    result = light(result, cyan, halo * envelope * 0.95);
    return light(result, ice, filaments * envelope * 2.0);
}
void main() {
    float p = clamp(progress, 0.0, 1.0);
    vec4 old = source(uv), fresh = target(uv);
    // Exact endpoints avoid a final-frame seam, including translucent windows.
    if (p >= 1.0) {
        color = effect == motion_disintegrate ? old : (isolated ? over(fresh, old) : fresh);
        return;
    }
    if (p <= 0.0) {
        bool starts_visible = effect == motion_disintegrate || effect == motion_hologram
                           || effect == motion_portal;
        color = isolated && starts_visible ? over(fresh, old) : old;
        return;
    }
    float eased = 1.0 - pow(1.0 - p, 3.0);
    float pulse = sin(p * 3.14159265);
    vec2 local = uv * resolution;
    vec2 delta = local - origin;
    if (isolated && (effect == motion_disintegrate || effect == motion_hologram
                     || effect == motion_portal)) {
        color = object_effect(local, old, p);
        return;
    }
    float radius = max(length(max(origin, resolution - origin)), 1.0);
    float distance = length(delta) / radius;
    float mask = eased;
    if (effect == motion_slide || effect == motion_scale) {
        vec2 q = effect == motion_slide ? uv - vec2(0, (1.0 - eased) * 0.06)
                            : (uv - 0.5) / mix(0.94, 1.0, eased) + 0.5;
        fresh = target(clamp(q, 0.0, 1.0));
        mask *= step(0.0, q.x) * step(q.x, 1.0) * step(0.0, q.y) * step(q.y, 1.0);
    } else if (effect == motion_iris) {
        float angle = atan(delta.y, delta.x) + (1.0 - eased) * 0.65;
        float blade = cos(mod(angle + 3.14159265 / 4.0, 6.2831853 / 8.0)
                         - 3.14159265 / 8.0);
        float opening = smoothstep(0.0, 1.0, p) * 1.09;
        float edge = distance * blade - opening;
        mask = 1.0 - smoothstep(-0.002, 0.002, edge);
        // Overlapping shutter leaves surround the aperture, then withdraw completely.
        float leaf = fract(angle * 8.0 / 6.2831853);
        float shade = (0.025 + 0.065 * leaf) * sin(p * 3.14159265);
        old = mix(old, vec4(vec3(shade), 1.0), pulse * 0.8);
        old = light(old, vec3(0.45), exp(-abs(edge) * radius / 2.0) * pulse * 0.3);
    } else if (effect == motion_ripple) {
        float wave = sin(distance * 28.0 - p * 12.0) * pulse * 3.0;
        fresh = target(clamp(uv + normalize(delta + vec2(0.001)) * wave / resolution, 0.0, 1.0));
        mask = 1.0;
    } else if (effect == motion_ink_bleed) {
        vec2 flow = local / 65.0;
        flow += vec2(turbulence(flow + 4.1), turbulence(flow + 19.7)) * 2.4;
        float field = turbulence(flow) * 0.7 + distance * 0.3;
        float front = p * 1.25 - 0.12;
        mask = smoothstep(field - 0.035, field + 0.035, front);
        float rim = exp(-abs(field - front) * 75.0) * pulse;
        fresh = mix(fresh, vec4(vec3(0.025) * fresh.a, fresh.a), rim * 0.72);
    } else if (effect == motion_particles) {
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
    color = isolated ? over(fresh * clamp(mask, 0.0, 1.0), old)
                     : mix(old, fresh, clamp(mask, 0.0, 1.0));
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
