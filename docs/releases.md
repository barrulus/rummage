# Release history and source versions

[Documentation](README.md) · [Current features](features.md) · [Installation](install.md)

rummage is a fork of Nordend. The current source version is 0.6.1; the source
commit identifies development builds more precisely than that number.
The development download tag is a moving snapshot, not a new upstream release.
See [installation](install.md#development-downloads) for package/source identity.

## 0.6.1 — in development

The information pane now shows a compact media summary, with extended metadata
under More details and a fixed Copy all properties button that includes collapsed
fields. Properties has a draggable scrollbar with a wider hit area, normal wheel
scroll speed and pixel-based touchpad scrolling.

Older unchanged sidebar menu configurations now pick up new Places and bookmark
actions automatically on load. Customised layouts remain intact, and no config
file rewrite is required.

Keyboard browsing now includes Alt+D for breadcrumb editing, Shift+F6 to focus
files, Ctrl+Enter for opening a folder in a new tab, and Ctrl+Alt+Enter for the
other pane. Places supports keyboard context menus and actions on its focused row.
Movement, hierarchy and explicit focus commands can be remapped; Shift/Ctrl
selection gestures follow movement bindings. A visible outline marks file-view
keyboard focus, and the new commands appear in Settings and the command palette.

The breadcrumb path editor completes local and archive paths with Tab/Shift+Tab;
Enter also reveals typed files. **Move into New Folder…** groups a selection with
Undo/Redo. Alt-drop offers destination actions and extraction of an archive into
a named subfolder. Places and bookmarks now have navigation context menus,
including **Open in New Tab** and **Open in Other Pane**.

Create symbolic links beside selected files and folders, with Undo/Redo.
**Ctrl+S** selects visible entries by filename fragment or wildcard pattern.
Mounted drives offer filesystem and space information in Properties and on hover.
**Use Default View for This Folder** resets and remembers a folder’s view defaults.

Fedora RPM builds and clean-install checks now target Fedora 45. Development
RPM download names and installation instructions use Fedora 45 consistently.

Settings translate the built-in action shortcuts and the menu editor's Remove button
in all bundled languages. Menu editor button rows use fewer columns when translated
labels need more room, including the Japanese Unnest button.

## 0.6.0

File pickers offer **Navigate in terminal…** for Bash, Zsh and Fish. Navigate in your
configured terminal, then exit the shell to return its folder to the picker.
Selection and confirmation remain in the GUI, with the original filters and mode.

Local bookmarks now use the selected system or named icon theme, including custom
`.directory` artwork and XDG special-folder icons. Missing artwork falls back to
the theme's folder icon, and the builtin theme retains Tabler bookmark glyphs.
Folder icon changes made through Properties also refresh bookmarks.

## 0.5.1

The fork includes system icon themes, static SVG file previews, HEIC/HEIF still previews,
visibility of unmounted internal partitions, optional isolated video
previews, configurable shortcuts/actions/menus, destination pickers and completion
notifications, optional PDF previews, archive password handling, expandable details, two persistent panes,
filename filtering, properties/folder icons, independent sidebar zoom, status text
settings, the file-picker portal and NixOS integration. Packages and public names
use rummage. Details columns can be shown, hidden and reordered, with sortable
creation/access times, owner, group, permissions, location and link target. Column
layouts are saved per folder and tab. See [current features](features.md).

ZIP, TAR, tar.gz and gzip archives can be browsed as read-only folders, with member
previews and copy-out extraction. Selected files and folders can be compressed to
ZIP or tar.gz. Existing external extraction backends remain available for other
formats and encrypted archives.

F2 on multiple selected items opens Batch Rename, with a live old/new preview,
literal replacement, templates and numbering. Conflicts prevent applying the batch;
sidecars follow their photos, and Undo/Redo applies to the whole batch.

Markdown opens rendered in Quick Look, with headings, emphasis, lists, quotes,
tables and highlighted code blocks. The visible toggle or M switches to source.
Links are styled text; images and external resources are not loaded.

The interface follows the system language and includes German. Translation catalogs
support language-specific integer plurals and separate meanings of the same English
word. File names and user-defined action labels keep their original spelling.

Quick Look keeps thumbnails and sharp previews at the same size when their original
dimensions are known, including rotated images from shared thumbnail caches. Video
stills and embedded raw previews can grow beyond their reduced decoding size.

## Upstream release history

The following entries summarize the user-visible changes in Nordend's releases.

## [0.5.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.5.0), 2026-11-09

Text previews: text files in Quick Look and the info pane, read in the caged helper
(UTF-8, UTF-16, Latin-1) with syntax colours from your theme for C, C++, Rust, Python,
JavaScript, TypeScript, Lua, Luau, shell (heredocs included), fish, CMake, Makefiles,
JSON, TOML, YAML, INI files and desktop entries, `.gitignore` and `.gitattributes`,
Markdown (code blocks in their own language) and diffs; select with the pointer, Ctrl+A,
Ctrl+C; long lines scroll sideways; Home, End, Page Up and Page Down; a caption with the
language, encoding and line ends; minified files shown plain; logs opened at their newest
lines. The helper became the first engine, described by a manifest

## [0.4.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.4.0), 2026-11-08

Image previews read by Nordend's own code: DNG developed when its previews are too small
(Leica M240 and M8, DJI drones, Adobe-converted files) with the file's colour, tone curve,
vignetting and lens corrections; TIFF
with its own reader (BigTIFF, LZW, Deflate, PackBits, JPEG, 8/16-bit and float, CMYK,
tiles, a scanner's scan in a SubIFD); raw files' previews and the info pane's EXIF without
exiv2; pictures made in small `nordend-preview`
processes, each in a cage of Nordend's own (seccomp and landlock), so a file that crashes
or hangs one costs only its preview; one path for every picture, told by the file's bytes;
a plain sentence when a file cannot be shown. Canon CRW and Minolta MRW get that sentence;
darktable sidecars' ratings are no longer read.

## [0.3.1](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.1), 2026-11-07

Fixes: DNG previews stored as a JPEG inside a TIFF (Leica M240, Samsung GX20 at full
size), broken previews passed over for the next one (Samsung SRW), PGM and PPM images, a
plain message for raw formats with no readable preview (Sigma X3F, old Panasonic RAW)

## [0.3.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.3.0), 2026-11-06

The command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>, fuzzy matching with fzy); finding files
under a folder (<kbd>Ctrl</kbd>+<kbd>F</kbd>); Nordend's icon follows the desktop's accent
colour (`icon.follow_accent`)

## [0.2.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.2.0), 2026-11-06

Sort By in the context menu; tabs (a folder, a view, a history and a zoom each,
remembered); zoom in the columns and the details

## [0.1.0](https://github.com/pankaspe/nordend-fm/releases/tag/v0.1.0), 2026-11-06

The first release: three views, raw previews and Quick Look, file operations with undo,
trash, drag and drop, disks, the desktop's look
