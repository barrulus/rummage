# Motion effects

[Documentation](README.md) · [Appearance](appearance.md) · [Configuration](configuration.md)

Rummage ships a bounded, event-driven motion layer. Open **Settings → Appearance**
to enable effects, adjust duration, select quality, or reduce motion. Changes apply
when saved; `config.toml` also reloads live. Input and filesystem work continue
while effects run. Repeated events replace the effect on that surface.

## Configuration

```toml
[animations]
enabled = true
duration_scale = 1.0
reduce_motion = "system"
quality = "auto"

[animations.events]
directory_open = "iris"
directory_back = "ripple"
view_change = "ink_bleed"
file_copy = "hologram"
file_move = "portal"
file_trash = "disintegrate"
selection_change = "energy_trace"
focus_change = "focus_glow"
invalid_operation = "glitch"
quick_look = "frost"
palette_open = "particles"
search_results = "scan"
```

| Setting | Accepted values and behavior |
| --- | --- |
| `enabled` | Boolean; `false` cancels all effects and releases capture textures on the next draw |
| `duration_scale` | Finite number from 0 to 4; default 1; 0 is immediate |
| `reduce_motion` | `system` (default), `always`, `never` |
| `quality` | `auto` (default), `low`, `high` |
| `events.EVENT` | Any built-in effect below, or `none` to disable one event |
| `durations.EVENT` | Integer milliseconds, 0–2000; 0 disables one event |

Scaled durations are capped at two seconds. Defaults are 220 ms for directory
open and Quick Look; 180 ms for history and search; 240 ms for view changes and
palette; 420 ms for file operations; 160 ms for selection; 140 ms for focus and
errors. The [complete example](config.example.toml) includes every duration.

`system` follows `org.freedesktop.appearance/reduced-motion` through the
[desktop Settings portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html),
including live changes. A missing portal/key means no preference. Use `always`
to suppress every new motion effect regardless of desktop support. This setting
does not change the existing wheel/touchpad scrolling physics or loading indicators.

## Built-in library

| Effect | Appearance |
| --- | --- |
| `fade`, `slide`, `scale` | Simple opacity, short vertical reveal, or slight expansion |
| `iris` | Circular reveal from the activated item's last visible bounds |
| `ripple` | Small radial displacement across the pane |
| `ink_bleed` | Smooth noise islands reveal the new view |
| `disintegrate` | A travelling operation icon erodes into speckles |
| `hologram` | Translucent icon with separated colour channels |
| `portal` | Travelling icon contracts, rotates and reappears |
| `energy_trace` | Fading outline and travelling light around the visible selected group |
| `focus_glow` | Brief soft highlight at the focused row or cell |
| `glitch` | Short, low-amplitude channel displacement |
| `frost` | The backdrop briefly softens as the overlay appears |
| `particles` | Fine noise particles resolve into the overlay |
| `scan` | A downward reveal with a thin accent edge |

The defaults distinguish pane transitions, local selection/focus feedback, overlay
reveals and travelling operation icons. Other event/effect mappings are allowed;
some effects, especially erosion and portal contraction, make most sense on icons.
Reveals composite current content against its backdrop, without retaining a stale
directory listing. These are compact built-in interpretations of the motion
concepts, not a particle physics simulation or a persistent frosted-glass surface.

## Operations and lifecycle

Copy/move/trash effects begin only after a successful result includes the captured
source item. Skips, errors, partial failures and cancellations do not produce a
success effect. A batch uses one representative icon. Geometry comes from the
visible source; operations without a visible captured item keep ordinary status
feedback. Destinations stay inside Rummage: the other pane, the visible Trash
place, or a status-area receipt for an offscreen destination.

Directory effects wait for the model to finish loading and check its error. Initial
startup does not animate the listing. Tab changes cancel pending surface effects;
resizing cancels active effects. Closing an overlay cancels its reveal. No effect
changes selection, focus, operation completion or undo history.

## Renderer and resource limits

The CPU scheduler has four channels: two panes, one overlay and one operation
sprite. Native geometry moves icons and draws focus/selection feedback. Other
effects use a cached GLES 3 program and two lazily allocated RGBA8 textures,
reused across passes. No effect requests frames when the scheduler is idle;
active frames use the window's compositor-paced redraw path.

`auto` and `low` cap texture storage at 16 MiB (two surfaces of at most 2,097,152
physical pixels each). `high` permits 32 MiB. Driver/program overhead is additional.
`low` substitutes fades. If the area exceeds the cap, allocation fails, or shader
compilation fails, normal content is drawn immediately. Shader failures are logged
and disable subsequent capture attempts for that renderer. Texture coordinates
are local, top-left origin; RGBA is premultiplied and sRGB-encoded, matching the
existing renderer. Captured backdrop composites replace pixels, avoiding a second
alpha blend on translucent windows.

Only built-in shader source is compiled. Community GLSL loading, manifests and
preset packs remain future work; a GPU shader is not a security sandbox.

## Shared implementation

`core/motion.hpp` defines the effect registry used by configuration names, native
feedback dispatch and generated GLSL constants. Shader branches use those named
constants rather than duplicated numeric IDs. `ui::MotionChannel` names the two
pane channels, overlay and operation channel.

`views/browser_motion.cpp` owns motion observation, invalidation, operation
provenance and render integration. The normal browser flow calls those helpers
at navigation, transfer and draw boundaries. Column, grid and details layouts
share `views::ItemBounds` to clip focused/selected item bounds and accumulate the
visible selection. The browser reads the same bounds interface for all layouts.

## Validation and profiling

Run `build-motion/tests/rummage-tests --test-case='motion:*'` for deterministic
scheduler, config, portal-policy, browser and offscreen shader tests. An EGL/GLES
context and fonts are required. With `RUMMAGE_MOTION_FRAMES=/tmp/motion-frames`
(create that directory first), the shader tests also write PPM stills for review.

Send `SIGUSR1` to a running Rummage process for capture bytes, allocation/pass/
fallback counts and last/maximum CPU frame-submission time. These timings exclude
GPU completion and compositor presentation; they are not frame-pacing or integrated
GPU performance claims. Compare hardware, scale, dimensions, theme, build mode and
revision when profiling. The texture counters exclude driver overhead.
