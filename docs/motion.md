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

## Quality and fallback

`auto` and `low` cap capture texture storage at 16 MiB; `high` permits 32 MiB.
Graphics-driver overhead is additional. `low` substitutes fades. When the area
exceeds the limit or a graphics resource cannot be created, normal content is
drawn immediately. Shader failures are logged and disable subsequent capture
attempts for that renderer. Idle effects do not request redraws.

Only the built-in effects are available. Loading custom shaders or preset packs
is not supported. For display problems, see [troubleshooting](troubleshooting.md).
