# Appearance

[Documentation](README.md) · [Configuration](configuration.md)

## Quick settings

Open the toolbar cog or press **Ctrl+,** for **Settings → Appearance**. Choose
a palette source (Automatic, Desktop, Palette file, Built-in or KDE) and a separate
colour mode (Follow source, light or dark); an icon theme; opacity; fonts; and
preview placement. The active-palette line reports the applied source, including
fallback and any light/dark override. Changes save automatically when valid; the
status reports the applied palette after saving completes. Font family changes
apply at the next start; text sizes and other appearance changes apply immediately.

The separate **Settings → Effects** section contains a slot selector, live preview,
parameters, quality and reduced-motion controls. See
[motion effects](motion.md) for per-event mappings, durations and resource limits.

## Colours

rummage accepts a palette from any producer: Noctalia, Matugen or a hand-written
file. Rendering and semantic roles remain owned by rummage; generators only write
data and are never launched by the application. No Qt, GTK or GLib integration is
needed for this interface. Fonts and transparency belong in `config.toml`.

With the defaults `appearance.palette_source = "auto"` and
`appearance.color_scheme = "system"`, sources have this precedence:

1. A valid `$XDG_CONFIG_HOME/rummage/colors.toml` (normally
   `~/.config/rummage/colors.toml`).
2. Desktop light/dark and accent preferences from the Settings portal.
3. The built-in dark palette when neither source is available.

The portal supplies preferences, not a standardized full application palette;
availability depends on the installed desktop portal backend. Desktop changes
apply live when Desktop is active. Select Desktop to follow those preferences
without removing a generated file. `--config` changes only the settings file,
not the palette path.

### Choosing a source

| Palette source | Config value | Behavior |
| --- | --- | --- |
| Automatic | `"auto"` | File, then desktop, then built-in; preserves existing precedence |
| Desktop | `"desktop"` | Desktop light/dark/accent preferences; ignores the palette file |
| Palette file | `"file"` | `colors.toml`; ignores desktop colours |
| Built-in | `"builtin"` | Built-in dark palette; ignores external colour sources |
| KDE | `"kde"` | Imports the active colours from `$XDG_CONFIG_HOME/kdeglobals` |

An unavailable explicit source falls back to Built-in, and Appearance explains
which source is unavailable. The saved preference stays selected, so a palette
file that appears later or desktop preferences that become available take effect
automatically. An invalid file update retains its last usable palette; it only
counts as unavailable if no valid file has been loaded or the file was removed.

For example, keep Matugen's output on disk while following the desktop:

```toml
[appearance]
palette_source = "desktop"
color_scheme = "system"
```

Switching sources never modifies or deletes `colors.toml`. The file and desktop
preferences keep updating in the background, so switching back uses their latest
usable values. The application accent icon also follows the selected source;
Built-in restores the installed icon when accent following is enabled. The source
choice affects colours only: desktop reduced-motion preferences and icon-theme
selection continue independently.

Choosing light or dark in Settings generates a palette from the active source's
accent (or the default blue when it supplies none). It does not select a
corresponding variant of an imported palette. Select Follow source
(`color_scheme = "system"`) to use the chosen source's colours and mode (with the KDE readability adaptation below).
Source selection and colour mode are independent and both reload live.

### Higher contrast

Rummage follows the Settings portal's
[`org.freedesktop.appearance/contrast`](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html)
preference at startup and while running. Higher contrast strengthens generated
text, accent colours and borders in both light and dark modes. Generated text
pairs target at least 7:1 contrast, and borders target 3:1 against the generated
surfaces. These ratios describe opaque palette colours; window transparency,
disabled controls and compositor effects can reduce the displayed contrast.

| Palette choice | Effect of higher contrast |
| --- | --- |
| Desktop, or Automatic using the desktop | Generates a higher-contrast palette from the desktop mode and accent |
| Any source with an explicit Light or Dark colour mode | Generates a higher-contrast palette from the selected source's accent |
| Palette file, or Automatic using a file, with Follow source | Keeps the file's colours exactly, including partial palettes and Matugen/Noctalia output; use a higher-contrast palette from its producer or choose Light/Dark |
| KDE with Follow source | Keeps the adapted KDE palette; choose a higher-contrast KDE scheme or Light/Dark |
| Built-in, including an unavailable source's fallback, with Follow source | Keeps the built-in palette; choose Desktop or Light/Dark to use generated higher contrast |

Missing, malformed or unknown contrast values mean normal contrast, as does a
portal backend that does not expose this setting. Returning to normal contrast
restores the normal generated palette. Contrast does not change source precedence,
the selected colour mode, or the palette file on disk.

### KDE colours

Choose **Settings → Appearance → Palette source → KDE** and **Follow source**, or set:

```toml
[appearance]
palette_source = "kde"
color_scheme = "system"
```

Apply a colour scheme in KDE's System Settings first. Rummage reads its active
colours from `$XDG_CONFIG_HOME/kdeglobals` (normally `~/.config/kdeglobals`),
including accent changes already written there by KDE. It does not need Qt or a
running KDE session. Automatic keeps its existing precedence; KDE is opt-in.
`--config` does not change this path. Neither KDE's file nor `colors.toml` is modified.

The adapter uses these active groups:

| KDE role | Rummage use |
| --- | --- |
| View background and alternate background | File content and alternate surfaces |
| Window background | Panes, popups and dialogs |
| Button background | Raised controls |
| View foreground, inactive, link and negative text | Normal, subdued, secondary and error text |
| Selection background and foreground | Selection, accent controls, drop targets and highlighted menu rows |

View, Window, Button and Selection must each contain `BackgroundNormal` and
`ForegroundNormal`. Optional roles fall back to readable derived colours. Values
must be opaque `R,G,B` or `R,G,B,255` with channels from 0 to 255. Rummage preserves
these backgrounds and the selection colour, but adapts foregrounds to target at
least 4.5:1 on opaque backgrounds. Window and button text share Rummage's normal
text role: View text is preferred, adjusted if needed. Schemes whose backgrounds
cannot use a common readable text colour are rejected. This is a palette adapter,
not a reproduction of Qt widget styling. Other semantic roles are generated from
the inferred light/dark mode and selection accent.

Only the materialized active groups are imported. Scheme-name lookup, KConfig
inheritance, colour effects and inactive-window groups are not applied. Immutable
`[$i]` markers are accepted; expansion/deletion modifiers on imported values are
rejected. Malformed, duplicate, incomplete, translucent or oversized (over 1 MiB)
input is rejected, with diagnostics in Appearance → Details.

Writes and atomic replacements reload live. An invalid update retains the last
valid KDE palette; a missing file falls back to Built-in. Creating it again resumes
KDE colours. Keep the containing configuration directory in place; when using a
symlink, replace the link or restart after changing its target elsewhere.

### Palette file interface

A minimal light palette is:

```toml
mode = "light"

[colors]
primary = "#6750a4"
```

`mode` accepts `"light"` or `"dark"`; omission means `"dark"` for compatibility.
The `[colors]` section is required but may be empty. Missing roles use a complete
baseline generated from that mode and the supplied `primary` accent (or the
built-in blue accent `#3584e4`). Supplied roles then override the baseline exactly.
When a supplied background omits its paired `on_*` foreground, that foreground
is adjusted for readability against the final background. Omitted `hover` follows
`primary`, and omitted `on_hover` is derived against the final `hover`.
The file's baseline does not depend on desktop preferences. A complete Noctalia
palette therefore retains all its colours, while a partial light file gets light
surfaces and dark text rather than missing roles from the built-in dark palette.

Every supported colour is a quoted `"#RRGGBB"` or `"#RRGGBBAA"` hexadecimal value;
letter case is immaterial and omitted alpha means opaque. Prefer opaque palettes
and use `appearance.background_opacity` for window transparency. Explicitly supplied roles
are never adjusted for contrast; provide readable pairs when supplying both roles.
Accent text and icons use a readable variant of `primary` on their actual surface,
while selection and button fills retain the exact supplied colour. Surface ladder
roles remain independent: overriding `surface` does not regenerate the other
surfaces. Since several surfaces share `on_surface`, keep those backgrounds
compatible with the same foreground when providing a custom ladder. Contrast
derivation assumes opaque colours; translucent roles depend on the backdrop.

| Roles | Purpose |
| --- | --- |
| `primary`, `on_primary` | Focused selections, accent controls and their foreground |
| `primary_container`, `on_primary_container` | Drop targets and their foreground |
| `secondary`, `on_secondary`, `secondary_container`, `on_secondary_container` | Secondary semantic accents and foregrounds |
| `tertiary`, `on_tertiary`, `tertiary_container`, `on_tertiary_container` | Additional semantic accents and foregrounds |
| `error`, `on_error`, `error_container`, `on_error_container` | Errors and destructive actions |
| `surface`, `on_surface` | Main background and text |
| `surface_variant`, `on_surface_variant` | Alternate surfaces and subdued text |
| `surface_dim`, `surface_bright` | Dimmer and brighter surfaces |
| `surface_container_lowest`, `surface_container_low`, `surface_container`, `surface_container_high`, `surface_container_highest` | Layered pane, control, popup and dialog surfaces |
| `outline`, `outline_variant` | Borders, separators and subdued details |
| `shadow`, `scrim` | Shadows and overlay dimming |
| `hover`, `on_hover` | Highlighted menu rows and their foreground; rummage-specific roles |

Additional keys within `[colors]` are ignored so producers can share a larger
palette. Unknown top-level keys produce a warning and are ignored. Missing roles
are valid and do not produce warnings. Invalid TOML, invalid `mode`, a missing or
non-table `[colors]`, or an invalid value for any supported role rejects the whole
file and logs its location. At startup this falls back to the desktop or built-in
palette; during reload it preserves the last usable palette, including its accent.
Settings retains the file diagnostics, distinguishing a missing file from an invalid
one and explaining when the last valid palette is still active.

### Live updates

rummage watches the palette's directory, coalesces changes briefly, and reloads
after writes close or a replacement is renamed into place. No restart, signal or
reload command is needed. Generators should write a temporary file in the same
directory and rename it over `colors.toml` to avoid exposing a valid but incomplete
partial palette. Invalid updates leave the current palette visible. Deleting the
file in Automatic restores the desktop or built-in fallback; in Palette file
it restores Built-in. Creating it again resumes file colours for those two
choices. Desktop and Built-in ignore file changes until you switch sources. Keep the containing directory in place while rummage is running.

## Matugen colours

Install [Matugen](https://github.com/InioX/matugen) separately, then copy the shipped
[template](../data/matugen/rummage.toml):

```sh
mkdir -p ~/.config/matugen/templates ~/.config/rummage
cp /usr/share/rummage/matugen/rummage.toml ~/.config/matugen/templates/rummage.toml
```

For a user install use `~/.local/share/rummage/matugen/rummage.toml`; from a checkout
use `data/matugen/rummage.toml`; for a local Nix build use
`result/share/rummage/matugen/rummage.toml`. Adjust paths for your install prefix
and `XDG_CONFIG_HOME`.

Add this entry to Matugen's `~/.config/matugen/config.toml` (create it with an empty
`[config]` section first if needed):

```toml
[templates.rummage]
input_path = "~/.config/matugen/templates/rummage.toml"
output_path = "~/.config/rummage/colors.toml"
```

Generate either mode, for example:

```sh
matugen color hex '#6750a4' --mode light
matugen color hex '#6750a4' --mode dark
# Or use a wallpaper as the colour source:
matugen image ~/Pictures/wallpaper.jpg --mode dark
```

The template maps Material roles directly, with `hover` from `primary` and
`on_hover` from `on_primary`. Matugen's selected mode supplies all 33 roles, so
none rely on rummage's missing-role defaults. Keep rummage's colour scheme on
Follow source (`color_scheme = "system"`) and select Automatic or Palette file to
follow these generated palettes. No Matugen `post_hook` is needed for rummage.
Other configured Matugen templates or hooks still run normally. Choose one generator to own the file; if Noctalia and Matugen
both write it, the last valid update wins when file colours are selected. See Matugen's
[configuration example](https://github.com/InioX/matugen/blob/main/example/config.toml)
for generator options.

## Noctalia colours

Install rummage, then copy its template into Noctalia's user template directory:

```sh
mkdir -p ~/.config/noctalia/templates ~/.config/rummage
cp /usr/share/rummage/noctalia/rummage ~/.config/noctalia/templates/
```

For a user install use `~/.local/share/rummage/noctalia/rummage`; from a checkout
use `data/noctalia/rummage`; for a local Nix build use
`result/share/rummage/noctalia/rummage`. Adjust these paths if using a custom
`XDG_CONFIG_HOME` or install prefix.

Add to Noctalia's `config.toml`:

```toml
[theme.templates.user.rummage]
input_path = "templates/rummage"
output_path = "~/.config/rummage/colors.toml"
```

Select a theme in Noctalia, even the current one, to generate the file. Later theme
changes update rummage live when Automatic or Palette file is selected with
Follow source colour mode. `--config` changes the settings file only; it does not
change the palette path.

## Fonts

In rummage's `config.toml`:

```toml
[font]
family = "sans-serif"
monospace = "monospace"
```

Values are Fontconfig patterns, for example `"Inter:weight=medium"` or
`"JetBrains Mono"`. rummage shapes text with HarfBuzz and uses font fallback.
Restart after changing fonts. Check resolution with `fc-match 'Inter'` if the
chosen family does not appear.

Glyphs are drawn the way GTK (Pango) and Qt draw them: the interface family's
Fontconfig `antialias`, `hinting`, `hintstyle` and `autohint` settings choose the
hinting and antialiasing, glyphs are placed in quarter pixels, and fonts with
size-dependent tracking (an AAT `trak` table) get it for the point size. Full
hinting and disabled antialiasing keep glyphs on whole pixels. Check the settings
with `fc-match -v sans-serif | grep -E 'hint|antialias'`; they apply at the next
start.

## Icon themes

```toml
[icon]
theme = "candy-icons"
follow_accent = true
```

`theme = "system"` follows the desktop theme from GTK/KDE/qt6ct settings at
startup and while running. An installed name selects it explicitly; `"builtin"` uses the bundled
Tabler icons. Theme icons may be SVG or PNG. Inherited themes and built-in icons
provide fallbacks. Editing this setting reloads live.

System discovery reads `gtk-3.0/settings.ini`, `gtk-4.0/settings.ini`, `kdeglobals`
and `qt6ct/qt6ct.conf` under `$XDG_CONFIG_HOME` (normally `~/.config`). On KDE,
`kdeglobals` takes precedence; elsewhere GTK 3, GTK 4, KDE and qt6ct are tried in
that order. Missing or invalid theme names fall through to the next source, then
`hicolor` and the bundled icons. Rummage follows writes, atomic file replacements
and configuration directories created after launch without creating those
directories itself. Only changes to the resolved theme refresh the window;
explicitly named and built-in themes ignore desktop changes. Returning to System
uses the latest desktop choice. Changes to icon assets within an unchanged theme
still require a restart.

SVG theme icons use librsvg through the same adapter as file previews, running
in-process. Input and output sizes are bounded; the preview helper's process
limits do not apply to icons. See [SVG support and limits](previews.md#svg).

### Places icons

```toml
[icon.places]
home = "builtin:home"
documents = "folder-documents"
pictures = "/home/you/.local/share/icons/my-pictures.svg"
```

Supported keys are `home`, `desktop`, `documents`, `downloads`, `music`, `pictures`,
`videos`, `filesystem`, `trash` and `trash_full`. They follow the purpose of an XDG
folder even when its path/name changes. `trash` covers both states unless
`trash_full` overrides the full state.

Values accept a theme icon name, `builtin:<name>`, or an absolute SVG/PNG path.
Built-in names are listed in [icons.txt](../third_party/tabler-icons/icons.txt) and
[icons-filled.txt](../third_party/tabler-icons/icons-filled.txt); filled names use
the `-filled` suffix. Theme names need a theme other than `builtin`. Missing icons
fall back to the usual icon. Changes reload live.

Local bookmarks use their target folder's `.directory` icon or XDG special-folder
icon with system/named themes, falling back to the theme's normal folder icon.
The `builtin` theme keeps the Tabler bookmark glyph. Changes made through
Properties refresh bookmark icons; restart rummage after changing them externally.

### Application accent icon

With `icon.follow_accent = true`, rummage writes an accent-coloured application
icon under `$XDG_DATA_HOME/icons/hicolor/scalable/apps/rummage.svg` (normally
`~/.local/share/icons/...`). It updates when the accent changes. Turning the setting
off removes rummage's generated override, or restores the installed icon for a
user-prefix install. An unrelated user icon at that path is left alone. Docks may
cache the previous icon until they refresh.

## Folder icons and properties

With system/named themes, folder icons use `Icon=` under `[Desktop Entry]` in the
folder's `.directory` file, shared with Dolphin. Values can be theme names,
absolute SVG/PNG paths or `./` paths relative to that folder. Standard XDG folders
use purpose-specific icons without an override; missing icons fall back. The
`builtin` theme retains Tabler icons. KDE's `EmptyIcon` behavior is not supported.
Reopen the parent folder after changing its icon externally.

Use **Alt+Enter** or **Properties** for a folder, then **Change Icon…**. Search the
paged theme icon grid or enter/paste a custom icon name/path. Default removes the
override. Choices are staged until Apply; Cancel leaves disk unchanged. Saving
preserves other `.directory` fields and refreshes open views. Trash properties
are read-only. Tab changes controls; arrows/Page Up/Down browse icons; Enter
chooses and Escape backs out or closes. Short windows scroll the content.

## Text size and sidebar zoom

Set `[status_bar]` with `text_size = 15` for larger status text (8–32 logical pixels,
12 by default). It reloads live and the bar grows to fit. Sidebar zoom is independent:
Ctrl+wheel over it, or F6 followed by Ctrl+plus/minus/0. File-list zoom remains per tab.

## Transparency

```toml
[appearance]
background_opacity = 0.70
```

This changes only backgrounds; text, icons and thumbnails remain solid. `1.0` is
opaque and `0.0` transparent. Keep compositor window opacity at `1.0`, since a
compositor opacity rule fades all content. rummage supplies transparency; any blur
comes from the compositor. The setting reloads live.

## Scale, decorations and pointers

Fractional scaling uses the compositor's fractional-scale and viewporter protocols.
The compositor can supply server-side decorations; otherwise rummage draws its own
title bar and resize edges. Cursor shapes use cursor-shape-v1 where available,
with an Xcursor fallback. See [compatibility](compatibility.md) for protocols,
fallbacks and diagnostic environment variables.

## Global interface size

Set `[font] size = 16.0`, or use Settings → Appearance → Interface text size. The range is
8–32 logical pixels; the default is 13. Text, controls, spacing and icons scale together
and update immediately. List, grid and sidebar zoom remain independent within that scale.
Font family changes still require a restart. The settings cog uses a bundled icon, so its
position does not depend on the chosen text font.
