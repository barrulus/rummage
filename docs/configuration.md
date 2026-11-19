# Configuration

[Documentation](README.md) · [Appearance](appearance.md) · [Actions](actions.md) · [Shortcuts](shortcuts.md)

Open **Settings** with the toolbar cog, **View → Settings**, **Ctrl+,**, or the
command palette. Categories appear in a sidebar, or a category selector in narrow windows:

- **General:** startup/session behavior, executable opening, double-click timing and terminal.
- **Appearance:** palette source, colour mode, icon theme, opacity, fonts, preview placement and motion.
- **Keyboard shortcuts:** search commands, record a chord, reset a binding to its default,
  or disable it. Conflicting chords are rejected before saving. Folder shortcuts accept
  a directory path; custom actions appear after their valid definition is saved.
- **Actions:** search or add commands, then edit the program and individual arguments,
  selection rules, destination prompt and enabled state. Placeholder help is shown in the form.
- **Menus:** choose the common layout or a context override, arrange commands, separators
  and groups, and preview the structure. Reset removes that override and restores inheritance.

Search above the categories matches setting names and descriptions, commands, actions and
menu contexts. Selecting a result opens its control. Individual reset buttons and **Reset
category** remove recognized overrides and restore the built-in defaults; they retain
unrelated settings and custom action definitions.

Copy Name defaults to **Ctrl+Alt+C** and can be changed with `copy_name`; it copies
selected filenames, including extensions, with newlines between multiple names.

**Valid changes save automatically.** Toggles and dropdowns commit immediately; text and
number fields commit on Enter or when focus leaves them. Numeric controls also support
plus/minus buttons and Up/Down while the field is focused. Settings stays open after saving.
The header reports saving, saved, editing or changes needing attention. Font family changes
require a restart; font size changes apply immediately.

Invalid input stays in its field with an error and **Details** for the complete message.
The last valid value remains active. New actions and shortcuts stay drafts until their
required fields are valid; unrelated valid preferences can still save. Create custom
commands in Actions before assigning their shortcuts.

**Close** or **Escape** commits pending valid edits and waits for writes. If edits cannot be
saved, Settings stays open: correct them, use **Retry** for a write failure, or explicitly
choose **Discard unsaved**. Discarding does not undo changes already saved.

Use Tab/Shift+Tab to move between controls, Alt+1/2/3/4/5 to switch categories, and
Ctrl+S to commit pending fields or retry a save. Scroll within a category with the wheel
or scrollbar. Choice fields open with Enter/Space; arrow keys move through options,
Enter/Space selects, Escape dismisses, and Tab dismisses and moves focus.

Existing comments and unrelated configuration are retained. Writes are serialized so quick
successive changes are retained. If another editor changes the file while Settings is open,
saving stops without overwriting that edit. Keep a copy of any unsaved values, discard those
edits and reopen Settings to load the external version. Invalid files and write failures
have readable error details. Settings uses the active `--config` file when specified.

When the resolved configuration destination cannot be replaced, Settings shows a **read-only**
state and disables editing and resets. For declarative configurations such as Nix-store files,
change the configuration declaration and rebuild it. **Open config** and copying values remain
available. A symlink to a writable destination still works: saves replace its target atomically
and preserve the symlink. Changes are applied only after a successful save; there is no separate
session-only settings mode.

rummage stores settings in TOML. Every configuration key is optional. The first
normal launch creates the complete reference when no config exists. First-run setup
never replaces existing files. You can keep a small file with only your changes, or copy the
[annotated example](config.example.toml) from this checkout.

## Files and locations

| File or directory | Default location | Ownership |
| --- | --- | --- |
| Settings | `~/.config/rummage/config.toml` | Edited by you or Settings |
| Palette | `~/.config/rummage/colors.toml` | Written by any palette producer (e.g. Noctalia or Matugen), or edited by you; see [Appearance](appearance.md#palette-file-interface) |
| Window/tab/folder state | `~/.local/state/rummage/state.toml` | Written by rummage |
| Picker window/view/last folder | `~/.local/state/rummage/picker.toml` | Written by the file picker |
| Thumbnail cache | `~/.cache/thumbnails/` | Shared freedesktop cache |
| Bookmarks | `~/.config/gtk-3.0/bookmarks` | Shared with GTK file managers |
| User folder definitions | `~/.config/user-dirs.dirs` | Desktop/XDG user-directory setup |
| Accent application icon | `~/.local/share/icons/hicolor/scalable/apps/rummage.svg` | Managed by rummage when enabled |

The corresponding XDG base-directory variables replace the usual defaults:
`XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and `XDG_DATA_HOME`.
`--config FILE` selects an alternative settings file. It does not relocate the
palette, saved state or caches; those still use their normal XDG locations.

From the repository root, to create a configuration when none exists:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/rummage"
cp -i docs/config.example.toml "${XDG_CONFIG_HOME:-$HOME/.config}/rummage/config.toml"
rummage --check-config
```

`cp -i` asks before overwriting an existing file. Alternatively, create a minimal file:

```toml
[appearance]
background_opacity = 0.85

[icon]
theme = "system"

[keybindings]
rename = "Ctrl+r"
```

## Settings reference

| Key | Default | Accepted value / behavior |
| --- | --- | --- |
| `startup.mode` | `"session"` | `"home"`, `"folder"`, `"last"`, or `"session"`; next launch |
| `startup.folder` | `"~"` | Absolute path or `~/` path, used with mode `"folder"` |
| `appearance.palette_source` | `"auto"` | `"auto"` (file → desktop → built-in), `"desktop"`, `"file"`, `"builtin"`, or `"kde"` (active kdeglobals colours); unavailable explicit sources fall back to built-in; live |
| `appearance.color_scheme` | `"system"` | `"system"` (follow selected source), `"light"`, or `"dark"`; independent of source; live |
| `appearance.preview_position` | `"right"` | `"right"` or `"sidebar"` (below Places); live |
| `appearance.background_opacity` | `1.0` | Number from 0 to 1; background only |
| `animations.enabled` | `true` | Enable built-in motion effects; live |
| `animations.duration_scale` | `1.0` | Finite number 0–4; 0 is immediate; final duration capped at 2 seconds |
| `animations.reduce_motion` | `"system"` | `system`, `always`, `never`; follows desktop reduced motion by default |
| `animations.quality` | `"auto"` | `auto`, `low` (fades), `high`; bounded capture storage |
| `animations.events.EVENT` | Per event | Built-in effect or `none`; see [motion effects](motion.md) |
| `animations.durations.EVENT` | Per event | Integer milliseconds 0–2000 before duration scaling |
| `status_bar.text_size` | `12.0` | Number from 8 to 32 logical pixels; applies live |
| `font.family` | `"sans-serif"` | Fontconfig pattern for interface text |
| `font.monospace` | `"monospace"` | Fontconfig pattern for text previews |
| `pointer.double_click_ms` | `400` | Integer from 100 to 2000 milliseconds |
| `pointer.double_click_distance` | `4` | Integer from 0 to 50 logical pixels |
| `icon.theme` | `"system"` | Desktop theme, installed theme name, or `"builtin"` |
| `icon.follow_accent` | `true` | Write the application icon in the current accent colour |
| `icon.places` | No overrides | Sidebar-purpose keys mapped to icon names or absolute paths |
| `keybindings` | Built-in bindings | Action names mapped to one chord; empty string disables |
| `actions` | Built-in registry only | Array of custom commands or extraction-preset overrides |
| `context_menu` | Built-in menus | Optional common or context-specific menu layouts |

See [appearance](appearance.md) for all place-icon keys, [shortcuts](shortcuts.md)
for binding names, and [actions](actions.md) for action fields and menu layouts.
The [example file](config.example.toml) supplies commented recipes and full default
menus. Its commented recipes are opt-in, not enabled defaults.

TOML table placement matters: use one `[keybindings]` table, one `[context_menu]`
table, and one `[[actions]]` table per action. A field below a table heading belongs
to that table until the next heading. Do not paste a binding into an action table.

## Validation and live reload

```sh
rummage --check-config
rummage --config /path/to/config.toml --check-config
```

Validation runs without opening a Wayland window. Status 0 means no problems;
status 1 means configuration or binding problems. A missing config is valid and
uses defaults, so verify the printed path if a check unexpectedly succeeds.

| Change | When it applies |
| --- | --- |
| Background opacity, status text size, pointer timing, icon theme/place icons/accent behavior | Live |
| Actions, menu layout and keybindings | Live, accepted together as a valid set |
| Startup folder/mode | Next application start |
| Interface and monospace font | Next application start |
| `colors.toml` palette | Live |
| Desktop icon-theme discovery | Live while `icon.theme = "system"`; named/built-in themes ignore desktop changes |

At startup, invalid scalar values use defaults and unknown keys are reported.
A TOML syntax error discards the file. Invalid bindings fall back to the default
binding set. During reload, a file that cannot parse leaves settings in use; bad
scalar values in an otherwise parsed file can revert those settings to defaults.
Invalid actions, menus or bindings retain the previous valid action/menu/binding
set. Diagnostics go to stderr; use `--check-config` before relying on an edit.

Deleting the config restores defaults on reload. The example file and your personal
copy are not automatically synchronized by updates.

## Saved state

rummage remembers window size/maximization, pane layout and divider, sidebar zoom,
info-pane settings, the divider below Places and command-palette history. Each pane retains its tabs,
locations, active tab, views, zoom, sorting, hidden visibility and filename filters.
Back/forward history and details-tree expansion are not saved across restarts.

View mode, sort key/direction, row size, grid icon size and details-column choices, order and widths
are remembered per folder. Opening a folder restores them. Moving focus between
Miller columns keeps column view without replacing the folder’s saved view mode;
choosing a view mode explicitly updates that preference. A folder without
saved settings initially inherits the current view. Sorting one tab does not
change other tabs. Existing tabs retain their own view and zoom until navigation;
when the same folder is used in several tabs, the last-used settings are saved
for future visits.

The folder history is capped at **512 entries**, with the least recently used
removed first. It stores paths and small settings records, never directory listings
or thumbnails, and adds no folder files, scanning or watchers. State snapshots
share the folder records until a setting or recency changes. Writes use the existing
one-second debounce on a worker and flush on exit. Settings are keyed by path; a renamed or moved folder gets a new entry.

Starting with an explicit directory always opens that directory. Otherwise,
`startup.mode` controls startup:

| Mode | Behavior |
| --- | --- |
| `"session"` (default) | Restore saved tabs and panes (including a hidden pane); otherwise open Home |
| `"home"` | Open Home in one tab and one pane |
| `"folder"` | Open `startup.folder` in one tab and one pane |
| `"last"` | Open the last active browser folder in one tab and one pane, including after a single-tab session |

```toml
[startup]
mode = "folder"
folder = "~/Projects"
```

For last-location startup, use `mode = "last"`. Missing configured/last folders
fall back to Home; session restore skips vanished tabs and uses Home for an empty restored pane. Trash does not replace the
last ordinary location. Picker windows use their requested location and do not
write browser state. Paths support `~` and `~/`, without environment-variable or
shell expansion.

State is application-managed, not an extra settings API. To reset it, close all
rummage windows, move `state.toml` aside and restart. Keep the copy to restore the
old layout. Resetting state does not change settings, bookmarks or your files.

## Migration from Nordend

A first normal launch without `--config` copies existing Nordend settings, palette
and state into rummage's XDG directories only where absent, preserving originals
and existing rummage files. Config checks, explicit-config runs and picker/portal
launches do not migrate. Commands and paths inside copied config stay literal;
update old executable names yourself. See [migration details](install.md#upgrading-from-nordend).

## Terminal and program opening

Settings → General includes the terminal program and its argument list. `[terminal]`
`program = "kitty"` and `args = ["--title", "Files"]` take precedence over `$TERMINAL`.
Leave the program empty for automatic discovery. Arguments are passed literally, without a shell;
Rummage adds the terminal's command-launch syntax. Changes apply immediately.
The file picker's [Navigate in terminal…](file-picker.md#navigate-in-a-terminal)
action uses the same terminal settings and an interactive Bash, Zsh or Fish from `$SHELL`.

`[open] programs = true` enables opening ELF executables and AppImages in their own folder.
An executable without permission asks before allowing and running; scripts continue to open
with their associated application. Set it to `false` in Settings or the configuration to disable
direct program launches. The file chooser never launches programs.
