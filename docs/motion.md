# Motion effects

[Documentation](README.md) · [Appearance](appearance.md) · [Configuration](configuration.md)

Rummage ships event-driven transitions and optional persistent shader effects. Open **Settings → Effects**
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
success effect. A batch uses one representative item. The effect retains the
thumbnail or icon actually displayed, with its size, proportions and decoration,
before the operation starts. It does not reload the removed file or substitute a
generic icon. Operations without a visible captured item keep ordinary status
feedback. Destinations stay inside Rummage: the other pane, the visible Trash
place, or a status-area receipt for an offscreen destination.

Directory effects wait for the model to finish loading and check its error. Initial
startup does not animate the listing. Tab changes cancel pending surface effects;
resizing cancels active effects. Closing an overlay cancels its reveal. No effect
changes selection, focus, operation completion or undo history.

## Quality and fallback

`auto` and `low` cap capture texture storage at 16 MiB; `high` permits 32 MiB.
Graphics-driver overhead is additional. `low` substitutes fades. When the area
exceeds the limit or a graphics resource cannot be created, normal content is
drawn immediately. Shader failures are logged and disable subsequent capture
attempts for that renderer. Finished transitions do not request redraws.

## Editing and previewing effects

Open **Settings → Effects** and choose the **Slot** you want to edit. The selector
contains all twelve event slots, plus **Background** and **Selection glow**.
The preview stays above the selected slot's controls while you scroll.

Event slots show an effect selector and duration. Additional controls appear for
hologram and portal effects. Persistent slots show their enable switch, shader
path where applicable, speed, intensity and frame rate. Global motion, quality
and reduced-motion controls follow under **All effects**.

Selecting a slot or changing a valid value immediately refreshes its preview.
Typing a valid numeric value updates the preview before you press Enter; incomplete
or invalid input keeps the last valid preview. **Replay** repeats the selected
slot. Pane, overlay and file-operation slots use corresponding sample scenes;
no files are changed. Persistent slots preview only the selected effect.
The preview respects global motion and reduced-motion settings. Changes save
with the normal Settings rules: choices immediately, text on Enter or focus change.

**Hologram spread** controls colour separation (0–12 physical pixels, default 2).
**Portal rotation** controls rotation at maximum contraction (0–12 radians,
default 2.5). These parameters affect their named effects wherever those effects
are mapped. Every event slot’s effect name and duration can be changed here.

## Persistent effects

Select **Background** and turn on **Enabled** for flowing accent colour behind
the file views, or enable **Selection glow** for a breathing outline around the
visible selection. Both are off by default. Each slot has its own live preview.

```toml
[animations.ambient]
background = true
selection_glow = true
speed = 1.0
intensity = 0.15
fps = 30
shader = ""
```

Speed accepts 0–4; zero freezes the effect. Intensity accepts 0–1; zero disables
persistent drawing. The frame-rate setting accepts 1–60 and caps redraws requested
solely by persistent effects; scrolling, input and short transitions can still
render at the compositor's rate. Persistent redraws pause when the window loses
activation. Disabled or reduced motion, including `duration_scale = 0`, suppresses
persistent effects too.

Background effects preserve window background opacity and draw beneath text and
icons. Selection glow follows the visible selection's bounding rectangle, including
groups of items. These effects do not allocate capture textures. Custom shader cost
still depends on its code and the number of pixels drawn.

## Custom background GLSL

Set `animations.ambient.shader` to an absolute path to a local `.frag` file and
enable `background`. An empty path selects the built-in background. Files are
checked every half second while the background is being drawn. Saving an edited
file, including atomic replacement, compiles a replacement shader. A failed read
or compilation retains the last working shader; before the first successful load,
the built-in background is used. The preview indicates errors and the log contains
the compiler diagnostics. With speed zero or an inactive window, reload is checked
on the next draw.

Write a GLSL ES 3.00 **function body file** with this entry point. Do not add a
`#version` directive, `main`, or declarations of the supplied uniforms:

```glsl
vec4 effect(vec2 p) {
    float band = 0.5 + 0.5 * sin(p.x * 12.0 + time_seconds);
    return vec4(accent.rgb, band);
}
```

| Input | Meaning |
| --- | --- |
| `p` | Local coordinates: top-left `(0, 0)`, bottom-right `(1, 1)` |
| `time_seconds` | Animation time in seconds, adjusted by speed |
| `resolution` | Target width and height in physical pixels |
| `accent` | Current theme accent as straight RGBA |
| `intensity` | Configured strength; the wrapper applies this to output alpha |

Return straight RGBA in the 0–1 range. The wrapper clamps the result, applies
intensity once and premultiplies it for compositing. The interface provides no
content textures or feedback buffers. Helpers and additional functions are allowed.
Use files you trust: custom GLSL runs in the application's graphics context, and
expensive shader code can make rendering unresponsive. Input is limited to regular
files of at most 64 KiB; this is not a GPU execution-time limit.

Custom object shaders, multipass effects and preset-pack manifests are not yet
supported. For display problems, see [troubleshooting](troubleshooting.md).
