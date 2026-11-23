# File operations

[Documentation](README.md) · [Using rummage](usage.md) · [Actions and archives](actions.md)

File operations are available through shortcuts, context menus and the command
palette. Errors and progress appear in the status bar. Right-clicking an entry
selects it unless it is already part of the selection.

## Open files and terminals

Enter or a double-click opens files using desktop MIME associations. Open With
lists associated applications, default first, and can open the selection with a
chosen application. Several files for the same app can share one launch. An
executable file is opened by its associated application; selecting it does not
implicitly execute it.

Applications run independently of rummage. Terminal applications use `$TERMINAL`,
`xdg-terminal-exec`, or a discovered emulator. **F4** opens a terminal in the
folder under the cursor, otherwise the current folder. The context menu also
offers Open in Terminal. Launch failures appear in the status bar.

## Create and rename

**Ctrl+Shift+N** creates a folder with its name ready to edit. The background
context menu's **New File** creates an empty file or copies a template from your
XDG Templates directory. It does not overwrite an existing file.

**F2** renames one entry in place, initially selecting the name without its
extension. Enter or a click elsewhere accepts; Escape cancels. Text editing
supports compose input, word movement, selection, clipboard paste and local
undo/redo. Paste uses the first line of clipboard text. While editing a name,
Ctrl+Z edits that text rather than undoing a file operation.

When the clipboard contains an image or plain text, **Ctrl+V** creates a file in
the current folder. Images retain their offered format (PNG, JPEG, WebP, GIF,
TIFF, BMP or SVG); plain text becomes a `.txt` file. Image data takes priority
over a text alternative. Clipboard file lists still copy or move the original
files as usual.

The new file starts as `Pasted.png`, `Pasted.txt`, or the matching extension, with
a number added if the name is taken. Its base name is selected for immediate
renaming: type a name and press Enter. Escape keeps the generated name. Creation
participates in file-operation undo. Pasting while a filename or other text field
is being edited continues to insert text into that field.

## Move into a new folder

Select entries and choose **Move into New Folder…** from the context menu or
command palette. Enter a folder name to create it and move the selection into it;
darktable sidecars follow their images. Existing folder names are refused.

**Ctrl+Z** moves the entries back and removes the new folder only if it is still
the same folder and is empty. Files added later are preserved. **Ctrl+Shift+Z**
repeats the grouping. If the folder was replaced, the operation is refused.
This action is unavailable in Trash, archives and file pickers.

Hold **Alt** while dropping files to choose an action at the destination. A drag
within rummage offers Copy Here, Move Here and Move into New Folder. Grouping also
works when dropping back into the source folder. Ordinary drops retain their
usual copy/move behavior.

For a single supported archive, Alt-drop also offers **Extract into New Folder…**.
Choose a name to extract into a subfolder of the drop destination. If that name is
taken, a numbered name is used. The original archive stays in place. Extraction
uses the usual progress, conflict, cancellation and Undo controls. Drops from
another application negotiate a copy so extraction does not remove its source.

## Create symbolic links

Select files or folders and choose **Create Link** from their context menu or the
command palette. rummage creates `Link to <name>` beside each source, choosing a
numbered name when needed, and selects the new links. Links point to the original
absolute paths, so you can move them elsewhere. Renaming or moving the originals
can break those links. Linked folders are not copied or scanned recursively.

**Ctrl+Z** trashes the new links without changing their targets; **Ctrl+Shift+Z**
restores the links. This action is unavailable in Trash, archives and file pickers.

## Batch rename

Select several entries and press **F2**, or choose **Batch Rename…** in the context
menu or command palette. The dialog previews old and new names as you type.
Choose **Target: Basename**, **Extension** (without its dot), or **Whole name**.
The dialog applies Find/Replace first, then the character edit, then case conversion,
then the template. Folder names and leading-dot names such as `.profile` have no
extension. Extension-only edits leave directory names unchanged. Removing an
extension also removes its delimiter dot.

**Find: Literal** performs case-sensitive literal replacement. Switch to **Find: Regex**
for [RE2 expressions](https://github.com/google/re2/wiki/Syntax). For example, find
`^IMG_([0-9]+)$` and replace with `Photo-\1` to turn `IMG_123.JPG` into `Photo-123.JPG`.
`\0` inserts the whole match, `\1`–`\9` insert capture groups, and `\\` inserts a
backslash. Inline `(?i)` enables case-insensitive matching. An empty Find disables
replacement. Invalid patterns or capture references disable Rename; lookaround and
pattern backreferences are unsupported. Regex compilation is bounded and matching
does not backtrack.

**Position (from 0)** and **Remove characters** choose a range in the selected
target; **Insert text** replaces that range. Position 0 inserts at the beginning;
a position beyond the end appends. For `aé😀z.txt`, position 1, removal 2 and insertion
`新` produces `a新z.txt`. Positions count Unicode scalar values, not bytes or grapheme
clusters: combining marks are separate positions. Invalid UTF-8 bytes count as one
position each and otherwise remain unchanged.

The **Case** control offers unchanged, lower, upper and title case for ASCII letters.
Non-ASCII characters are preserved, and no normalization is performed. For example,
lowercase on the basename turns `PHOTO.JPG` into `photo.JPG`; on the whole name it
produces `photo.jpg`. Title case capitalizes ASCII words separated by punctuation
or spaces.

| Template token | Value |
| --- | --- |
| `{name}` | Transformed basename |
| `{ext}` | Transformed extension including its dot |
| `{n}` | Sequence number with the chosen padding |
| `{date}`, `{time}` | UTC date (`YYYY-MM-DD`) and time (`HH-MM-SS`) when the dialog opened |
| `{mtime}`, `{mtime_time}` | File's modification date and time in the same UTC formats |
| `{size}` | Regular file's byte size |

Metadata is a snapshot taken while preparing the preview. Missing requested metadata
disables Rename, rather than inserting an empty value; `{size}` is unavailable for
directories and symlinks. These are filesystem metadata tokens, not EXIF, media tags
or darktable-sidecar metadata. The date/time used for the batch stays fixed while
editing the preview.
For example, `bug-report_{n}{ext}` with start 1 and padding 2 produces
`bug-report_01.png`, `bug-report_02.png`, and so on. Numbering follows the current
view order; sort before opening the dialog. Step must be positive. Names must fit
within 255 bytes, and replacement/insertion fields within 255 bytes each.

Recognized darktable sidecars follow their photo, including duplicate sidecars.
Selecting them together with the photo does not consume extra numbers. The
preview includes their destination names. Invalid names, duplicate targets and
names occupied by unrelated entries disable Rename. Changes are checked again
when applied; files replaced since the preview are refused.

Tab moves through the fields, transformation controls, preview and buttons.
Space or Enter cycles a transformation control; Left/Right cycle backwards/forwards.
Enter elsewhere applies, Escape cancels,
and Page Up/Down or the wheel scrolls the preview. **Ctrl+Z** undoes the entire
batch, including its sidecars; **Ctrl+Shift+Z** redoes it. Swaps and chains use a
private temporary directory, and no step overwrites an existing entry. A failure
rolls back completed moves. If another process prevents rollback, a persistent
error dialog lists the remaining files and their recovery paths. Filesystems
without atomic no-replace rename support refuse the operation. Batch rename is
unavailable in Trash, archives and file pickers.

## Copy, move and conflicts

**Ctrl+C / Ctrl+X** copies/cuts the selection; **Ctrl+V** pastes into the current
folder. Cut entries fade until pasted or cancelled with Escape. File-manager
clipboard formats allow exchange with other desktop applications; only local
file paths/URIs are accepted for transfers.

Transfers run outside the UI thread, with progress and a stop button.
**View → Verify copied contents**, or **Enable Copy Verification** in the command
palette, enables verification for subsequent copies and moves in this window.
It defaults to off, is not saved across restarts, and each queued transfer keeps
the setting it was submitted with.

For regular files, including nested files and sidecars, verification flushes the
staged destination and rereads both files in bounded chunks for a byte-for-byte
comparison. This compares the contents directly rather than comparing checksum
digests. Copy and verification byte counts are shown separately; parallel files
can be copying and verifying at the same time. Verification adds two reads of the
data and a flush, which can be expensive on slow or network storage.

A failed or cancelled comparison does not publish that file or replace an existing
destination. Cross-filesystem moves retain unverified source files. Source identity,
size and modification/change timestamps are checked, including before deletion;
an externally changed source is reported instead of claiming success. As with other
partial transfers, successfully completed files may remain at the destination.
This is a consistency check through the operating system, not a guarantee against
future media failure or changes made after the check.

Same-filesystem moves remain renames and do not reread data. Symlinks copy their
link target without following it; their referent contents are not verified.
Archive extraction does not support this option and reports that limitation until
verification is disabled. With verification off, existing transfer behavior is unchanged.

**Shift+Escape** stops the active transfer from a file view or Quick Look; the
command palette also offers **Stop the Transfer** while work is pending. The
`stop_transfer` binding can be changed in Settings or `config.toml`.

Copies use temporary names before publishing completed output. Files can copy
in parallel through a bounded queue, while conflict questions remain on the UI
thread. A cross-filesystem move removes its source only after its copy is flushed
and published. Dates, permissions and symbolic links are preserved, and supported
darktable sidecars travel with the photo.
Moving a folder into itself or copying more than the available space is refused.

When a destination name exists:

| Choice | Result |
| --- | --- |
| Replace | Replace a file after its new contents are complete |
| Merge | Join two folders, asking separately about conflicting files inside |
| Skip | Keep the destination and omit that source |
| Keep both | Give the incoming item a numbered sibling name |
| Apply to the rest | Reuse the selected policy; a folder-merge choice never authorizes replacing files |

Nested folders join as part of the requested merge. Existing files inside them
are still subject to file-conflict choices.

Pasting into the source folder automatically keeps both. Cancellation stops work;
items already completed can remain. Temporary handling does not make a multi-file
operation one atomic transaction. Check the status message for partial failures.

## Drag and drop

Drag an entry by its icon or name. A selected entry drags its selection. Targets
include folders in each view, view background, breadcrumb segments and sidebar
places; dropping onto Trash trashes the files.

Within rummage, the default is move on the same filesystem and copy across
filesystems. Ctrl requests copy; Shift requests move. It uses the transfer engine,
including conflict handling, sidecars, progress and undo. Dropping into the same
location has no effect; a folder cannot be dropped inside itself.

External drops into rummage copy local files. Drops out of rummage offer file URIs
and paths for copying; rummage does not delete its source because another program
claims it moved it. Web URLs are not local files. Dragging near a view's edge
scrolls; hovering does not automatically open a folder.

## Trash and restore

**Delete** moves files to freedesktop Trash. Files on the home filesystem use the
home trash; other filesystems use their own trash where permitted. If a trash
cannot be created, the files remain and rummage reports why; it does not silently
replace trashing with permanent deletion.

The sidebar's **Trash** combines entries across the discovered trash locations.
It shows original names, original paths and deletion dates, with previews. Files
cannot be opened, created, renamed or pasted there.

Right-click **Restore** to return an entry to its original path, recreating missing
parent directories and asking about name conflicts. Restore does not merge a
conflicting directory. Supported trashed sidecars return with their photo.

## Permanent deletion

**Shift+Delete** permanently deletes from a normal folder. **Delete** in Trash
also means permanent deletion. **Empty Trash** is available from its background
or sidebar menu and covers the discovered trash locations.

These actions ask for confirmation. Folder contents are counted for the size and
progress display. Deletion runs in the background and can be stopped, but files
already deleted are gone. Symbolic links are deleted without following them.
Permanent deletion cannot be undone.

## Undo and redo

**Ctrl+Z** undoes the last supported operation; **Ctrl+Shift+Z** redoes it.

| Operation | Undo behavior |
| --- | --- |
| Rename | Restore the previous name, including associated sidecars |
| Move | Move back, including across filesystems |
| Trash | Restore |
| Restore | Trash again |
| Copy | Trash the copies |
| Create symbolic links | Trash the links, leaving their targets untouched |
| Create folder/file | Trash it only if still empty/unchanged as appropriate |
| Permanent delete | Cannot undo |
| External action or archive extraction | Not recorded in file-operation undo |

Undo is session history, not a backup system. Changes made meanwhile can prevent
an inverse operation; rummage skips affected items and explains the result.
After a folder merge, undo affects only items newly placed by that transfer and
leaves pre-existing destination files in place. Undoing a merged move recreates
an emptied source folder when needed. Replaced files cannot be restored. A photo
with pre-existing destination sidecars is also excluded from undo so those
sidecars remain safe. Undo reports how many items cannot be safely undone and
leaves those items in place.

## darktable sidecars

Recognized darktable `.xmp` sidecars follow their photo through rename, copy, move,
trash and restore, including conflict-generated names. This is file handling,
not a darktable database integration. The current photo reader does **not** import
darktable star ratings; older documents described behavior removed in 0.4.0.

For compression, extraction, image conversion and other external tools, see
[actions and context menus](actions.md). External commands own their output and
overwrite behavior; they do not inherit the built-in transfer engine's guarantees.

## Executable files

For programs and scripts, **Allow Running as Program** adds owner execute permission;
**Stop Running as Program** removes execute permissions. These changes support Undo/Redo.
A small icon badge and “Runs as program” in the information pane indicate executable types
with permission. Direct opening runs ELF programs/AppImages, not scripts or documents renamed
to look like executables; a filesystem mounted `noexec` reports why running is unavailable.

## File permissions

Open **Properties** to change owner, group and others' read, write and execute
permissions on a file or folder you own. For folders, execute permits traversal.
Changes are staged until **Apply**; **Cancel** discards them. The operation affects
only that item, not its children, and uses your current user permissions without
elevation. Symbolic links and read-only archive items do not offer permission edits.
Ownership and special permission bits are not editable. Filesystem errors and
changes made externally while Properties was open leave the dialog open with an error.
