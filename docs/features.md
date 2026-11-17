# Feature overview

[Documentation](README.md) · [Using rummage](usage.md) · [Release history](releases.md)

This is a map of the current source tree's capabilities. Detailed behavior belongs
in the linked guides. The fork develops beyond upstream Nordend 0.5.0; see [release history](releases.md).

## Browsing and navigation

- Columns, thumbnail grid and a details table; changing views retains location and selection.
- Expandable folders in details, loaded on demand; selection stays within one parent.
- Two persistent panes with their own tabs, locations, views, sorting, hidden settings and filters.
- Live filename filtering alongside recursive search; independent sidebar zoom.
- Sorting by name, size, modified date or type; hidden-file toggle; live directory updates.
- Keyboard and mouse selection, rubber bands, smooth scrolling and adjustable zoom.
- Breadcrumb, history, places, GTK bookmarks and removable-device integration.
- Network bookmarks (smb, sftp, ftp, WebDAV) mounted through gvfs; Ctrl+L to go to a folder or server.
- Fuzzy command palette and recursive filename/path search with cancellation.

See [Using rummage](usage.md) and the [shortcut reference](shortcuts.md).
Search is not content search; remote browsing without gvfs and encrypted-volume
unlocking are not implemented. Local bookmarks can be renamed, repointed and
reordered. Name sorting is natural and ASCII
case-insensitive, not locale collation.

## Files and external applications

- Copy/move with progress, cancellation and conflict choices; rename and create from templates.
- Batch rename with a live preview, literal replacement, templates and numbering in view order;
  darktable sidecars follow, and the batch has one Undo/Redo step.
- Freedesktop Trash across filesystems, restore, confirmed permanent deletion and undo/redo.
- Clipboard exchange and drag/drop with desktop applications.
- Mounting internal partitions and removable disks through udisks2, with authorization when required.
- Properties and a folder-icon picker, sharing `.directory` metadata with Dolphin.
- Default applications and Open With from desktop MIME associations; terminal launching.
- darktable sidecars follow supported photo operations.
- Copy Location, automatic archive backends and configurable external actions.
- Destination pickers, completion notifications and nested/context-specific menus.
- Browse ZIP, TAR, tar.gz and gzip archives, preview their members, copy files out,
  and create ZIP or tar.gz archives from the selection.

See [file operations](file-operations.md) and [actions](actions.md). Permanent
removal cannot be undone; external commands/extraction are outside file-operation
undo. File handling is local; general queued jobs and macros are not implemented.
Archive contents are read-only; extract files before changing them.

## Previews

- Isolated image and text decoding through `rummage-preview`.
- PNG, JPEG, GIF, WebP, JPEG XL, HEIC/HEIF (HEVC), TIFF/BigTIFF and binary PGM/PPM previews.
- Static SVG previews and thumbnails using the built-in renderer in the helper.
- Camera raw embedded previews and development of supported DNG data when needed.
- Photo metadata, orientation/crop handling and shared freedesktop thumbnail caching.
- Quick Look for pictures and text, with file-to-file navigation.
- Text encoding detection, syntax colours, selection/copy and log tails.
- Rendered Markdown in Quick Look, with a source toggle, headings, lists, tables,
  emphasis and highlighted code blocks.
- Optional isolated FFmpeg video thumbnails and Quick Look stills.
- Optional isolated PDFium first-page thumbnails and Quick Look stills for PDFs, with lock and
  damage badges.

See [previews and formats](previews.md) for decoder limits. Video playback and
darktable rating import are not current capabilities. SVG theme icons
use the same renderer in-process; SVG file previews use the sandboxed helper.

## Desktop integration and customization

- Folder-jump shortcuts, configurable startup locations, and per-folder view/sort/zoom
  preferences for up to 512 recent folders.

- Wayland input, clipboard, drag/drop, activation and fractional scaling where protocols exist.
- Server-side decorations or rummage's title bar; cursor-shape or Xcursor fallback.
- Desktop light/dark/accent settings, generic partial/full palettes, Noctalia and Matugen templates, and live palette changes.
- Desktop SVG/PNG icon themes, custom place icons and an accent application icon.
- Fonts, pointer settings, background opacity, keybindings, actions and menu layouts in TOML.
- Remembered two-pane/multi-tab sessions and window/view state, without saving navigation history.
- Optional read-only [git status](git.md): branch, ahead/behind and changes in the status bar,
  and badges on changed, untracked, conflicted and ignored files.

See [configuration](configuration.md), [appearance](appearance.md) and
[compatibility](compatibility.md). Open **Settings** with **Ctrl+,** for appearance,
keybindings and actions. Font family changes need a restart; font sizes apply
live. Other settings have the reload rules listed in the configuration guide.

## Show in Folder

- Optional `org.freedesktop.FileManager1` integration for browsers and other applications.
- Reveal local items selected in parent-folder tabs, or open their expanded information.
- Open multiple folders as tabs, keeping the first folder focused.
- User-local setup/removal commands preserve another application's service file.

See [Show in Folder setup and commands](command-line.md#show-in-folder). This is
independent of the FileChooser portal and the default folder application; remote
URIs are unsupported. Each request opens a new window.

## File selection for other applications

- Standalone open-file, multi-file, directory and save-file commands with NUL-separated output.
- Optional XDG FileChooser backend for OpenFile, SaveFile and SaveFiles.
- Glob/MIME filters, application choices, overwrite confirmation and cancellation.
- Shared views/previews while keeping the browser's saved session separate.
- D-Bus activation, Wayland parenting where available and NixOS module configuration.

See [file-picker setup](file-picker.md) and [command-line reference](command-line.md).
Only applications using the portal use this backend. Unsupported parent handles
open independent windows; filename-based MIME filtering has limits.

## Runtime and security

rummage uses C++20, Wayland and OpenGL ES without GTK, Qt or GLib. The
[security model](security.md) distinguishes isolated readers from other file
handling and external programs. See [installation](install.md) for runtime and
build requirements.
