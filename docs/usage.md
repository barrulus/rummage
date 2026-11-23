# Using rummage

[Documentation](README.md) · [File operations](file-operations.md) · [Shortcuts](shortcuts.md)

Start `rummage` in a Wayland session, or pass a directory such as
`rummage ~/Pictures`. **F1** shows the active keyboard bindings and **Ctrl+K** opens
the command palette. Both reflect configured actions and shortcuts.

## The window

The sidebar contains places, bookmarks, Trash and devices. The breadcrumb above the
files opens any parent in the current path. The status bar shows counts, selection,
free space, operation progress and messages, plus view buttons. The information
pane shows a preview and a compact metadata summary for the selected item; expand
More details for extended media fields, permissions, owner/group and links.
Copy all properties at the top includes collapsed fields.

Right-click a place or bookmark to open that location, open it in a new tab or
in the other pane, copy its path, or view Properties. Open in Terminal appears when
a terminal is configured. Bookmark rename, reorder and removal commands remain in
the same menu. Unmounted devices offer mounting first; path actions become available
after mounting.

Keyboard browsing highlights the focused Places row or outlines the active file
area. **Shift+F6** returns to the file list. In Places, **Menu / Shift+F10** opens
the focused row’s menu, **Ctrl+Enter** opens it in a new tab, and **Ctrl+Alt+Enter**
opens it in the other pane. **Alt+D** edits the breadcrumb path directly.
See [keyboard browsing](shortcuts.md#browse-entirely-with-the-keyboard) for remapping
movement, focus, opening and hierarchy commands.

**Ctrl+B** toggles the sidebar. **F6** moves keyboard focus into it and back;
arrows choose a place, Enter opens it and Escape returns to the files.
**Ctrl+I** toggles the information pane. Drag its edge to
resize it (240–720 logical pixels). Filenames and metadata wrap in full. Drag to
select text in the pane, double-click a word, triple-click a line, or Shift+click
to extend. After clicking a text field or text preview, **Ctrl+A** selects its text
and **Ctrl+C** copies it. Click the file view to return these shortcuts to files.

Rest on a selected folder for 250 ms to count its recursive size on a worker. The
pane shows “Counting…” with the growing size, then Size and Contains. Selections
containing folders get a combined recursive size; files alone show their listed
sizes immediately. Moving to another selection or hiding the pane cancels the
count. Hidden files are included, hard-linked file bytes count once, and links
inside folders are never followed. A selected link to a folder measures its target;
selecting both that target and its link counts the directory once.

Counts stay on each root's filesystem and stop before directories 64 levels deep.
Unreadable entries and skipped mounts make the size say “at least”. Network and
FUSE folders are skipped (“—” when nothing could be counted); local fuseblk disks
such as NTFS and exFAT are counted. Sizes reflect the tree when it was measured;
changes deep inside it may require selecting it again to recount.

## Choose a view

| View | Shortcut | Behavior |
| --- | --- | --- |
| Columns | Ctrl+1 | Selecting a folder opens the next column; Left returns through the hierarchy |
| Grid | Ctrl+2 | Icons/thumbnails with labels; useful for pictures and videos |
| Details | Ctrl+3 | Configurable property columns; folders can expand inline |

Switching views keeps the current location and selection. Columns can be resized
by dragging their edges; double-click an edge to reset its width. In details,
drag header dividers to resize columns and click a header to sort by it; click
again to reverse. Right-click a header or use **View → Show columns** to toggle
columns, including Type. Name always stays first. The header menu also moves
an optional column left or right and resets the columns and widths.

Available columns are Name, Size, Modified, Type (MIME), Created, Accessed, Owner,
Group, Permissions, Location and Link target. Creation time shows a dash when
unavailable; permissions use `ls -l` notation. Links show their target's metadata,
or their own when dangling. Location is the row's parent folder, useful in an
expanded tree; sorting it orders siblings by name. Drag a header’s right edge to
resize that column; the columns to its right move with it. Wider tables scroll
horizontally beneath the fixed preview pane. Use the scrollbar at the bottom of
the file list, Shift+wheel, or a horizontal touchpad gesture to reach offscreen columns.
Column choices, order and widths are remembered per folder and tab. The command
palette also offers **Show/Hide … column** commands in Details view.

Drag a scrollbar thumb to move quickly through a long folder without changing the
selection. Each column, grid and details pane has its own vertical scrollbar;
columns also have a horizontal scrollbar when they extend beyond the pane. Click
the scrollbar track to jump to that position, then keep dragging to adjust.

In every view, use the toolbar’s **Sort By** button for any of these properties and
ascending/descending order. It is also available in **View → Sort By** and the
right-click menu. Sort order belongs to the current tab and is saved. Folders
are kept together; size sorting keeps folder names ordered, and unknown types
sort after known types in ascending type order.

**View → Show → Thumbnails** replaces file-type icons with thumbnails where supported in
Grid, Details and Columns. Turn it off for icons only. The choice applies to every
pane and tab and is remembered between sessions.

**View → Show → Hidden files** shows or hides dotfiles using the same setting as **Ctrl+H**.

**View → Show → File extensions** shows or hides the final extension of file labels in
all panes and tabs; the choice is remembered between sessions. Folder names and
leading-dot names such as `.profile` stay intact. Files on disk keep their names.
Long labels end in an ellipsis. Selecting a single file shows its complete name,
including its extension, in the status bar even when the preview is hidden.
Labels stay on one line and truncate to fit in every view. The status bar gives
the filename priority over disk space; names wider than the bar are ellipsized.
The preview and Quick Look also show the complete name. Rename always edits the
real filename, with only its base name initially selected.

Use **Ctrl+plus**, **Ctrl+minus**, or **Ctrl+wheel** to zoom, and **Ctrl+0** to reset.
Grid sizes range from 48 to 512 px, constrained by available width. Columns and
details share a five-step list zoom; grid has its own zoom. Zoom is per tab.

### Expand folders in details

Click a folder's chevron or press **Right** to expand it. Right again moves into
its first child. **Left** collapses a folder or selects the parent row. **Enter**
or a double-click opens the folder as the new location; **Backspace** goes above
the tree root.

Expanded directories load on demand and follow filesystem changes, sorting and
hidden-file settings. Selection stays within one parent folder, including Shift
ranges, Ctrl+A and rubber-band selection. Expansion survives switching tabs, but
resets on navigation or switching views and is not saved across restarts.

## Navigate and select

Enter or a double-click opens a folder, or opens a file in its default application.
Backspace or Alt+Up goes to the parent. Alt+Left/Right and mouse side buttons go
back/forward through location history. Click an ancestor breadcrumb to navigate. Click the current folder segment or empty
space in the breadcrumb to edit its path inline. The current path starts selected:
copy it with Ctrl+C or paste a new path with Ctrl+V. Enter navigates; Escape cancels.
Tab completes a path; press Tab again to cycle forward, or Shift+Tab to cycle
backward. Completion includes files, folders and paths inside supported archives.
Type a leading dot to include hidden names. Absolute paths, paths relative to the
current folder, `~/` and local `file://` URIs are supported. A completed URI becomes
a decoded local path. Enter opens a folder or reveals a file in its parent folder.
An invalid path keeps the field open and shows an error in the status bar.

Arrow keys move the cursor; Page Up/Down moves by a screen and Home/End reaches
the first/last item. Grid Left/Right moves between cells; columns and details use
those keys for hierarchy navigation.

| Gesture | Selection |
| --- | --- |
| Click | One item |
| Ctrl+click or Ctrl+Space | Toggle an item |
| Shift+click or Shift+movement | A range |
| Ctrl+movement | Move the cursor without selecting |
| Ctrl+A | All visible entries in the current selection scope |
| Escape | Return to one entry, or cancel the active interaction |
| Drag from empty space | Rubber-band selection; Ctrl adds to it |

A drag beginning on an icon or name starts a file drag instead. Rubber-band
selection scrolls when you move beyond the view's top or bottom. Selection and
cursor track entries through live directory updates.

**Ctrl+S** opens **Select by Pattern…**. Enter a filename fragment or a wildcard
pattern such as `*.ORF`, `IMG_??.jpg` or `[ab]*`; matching ignores case. Choose
**Select** to replace the selection, or **Add to the Selection** to extend it.
Only visible entries in the current selection scope participate, including any
active filename/type filter. An empty pattern selects nothing. Spaces can be
typed normally; Escape leaves the selection unchanged. This is also available
in Open file pickers, but not Save pickers.

**Ctrl+H** toggles hidden files in the current tab. Hidden entries are dimmed when shown;
when hidden, they are excluded from Select All and operations on that selection.

The back and forward buttons before each pane’s path navigate that pane’s history.
They remain visible when the compositor provides the title bar.

## Tabs and persistence

**Ctrl+Shift+T** or **Reopen Closed Tab** in the command palette restores the last
closed tab in the active pane, at its previous position, with its view, zoom,
history, filter, hidden-file setting and details columns. Each pane keeps its last
ten closed tabs until the window closes. A folder that has disappeared is reported
and skipped; press again to restore the preceding tab.

**Ctrl+Shift+I** inverts selection among visible entries. Hidden files and entries
excluded by the filename or picker filter stay unselected.

**Ctrl+T** opens a tab, **Ctrl+W** closes the current tab (except the last), and
**Ctrl+Tab / Ctrl+Shift+Tab** cycles tabs. **Alt+1…9** selects a numbered tab.
The tab bar appears when there is more than one tab. Click to select, use its ×
or middle-click to close, and use + to open another.

Middle-click a folder to open it in a background tab. If it is already part of a
selection, the selected folders open in background tabs. The context menu offers
Open in New Tab as well.

Each tab keeps its location, view, zoom, sort order, hidden visibility, filename
filter and session history. The sidebar and information pane are shared by the
window. New tabs initially inherit view/zoom/sort/hidden settings and start with an empty
filter. Saved folder preferences take precedence when navigating: view mode, sort,
zoom and details-column choices, order and widths are remembered for up to 512 recent locations. Pane/tab restoration is described in [saved state](configuration.md#saved-state).

The **Sort By** menu marks a **Custom folder view** when the active folder differs
from the built-in view defaults. Choose **Use Default View for This Folder** there
or in the command palette to reset view mode, sorting, zoom and details columns
for that folder. The reset is remembered on later visits and restarts; other
visible tabs and panes keep their current appearance. Filters and hidden-file
visibility are unchanged. This action is unavailable in file pickers.

## Settings and preview placement

The existing filter toolbar contains **View** and a **cog** for Settings; pane
controls no longer occupy a separate row. View includes display modes, pane
commands, preview/sidebar visibility and Settings. Pane commands also remain in
the right-click menu and command palette, with configurable shortcuts.

In **Settings → Appearance**, set **Preview placement** to **Below Places** to
share the sidebar with the active pane's preview. Drag the horizontal divider to
adjust the space; double-click to reset it. Places and preview scroll independently,
and the divider is remembered between launches. **Ctrl+I** hides either placement.
If Places is hidden or the window is too short, the preview uses its normal right
side location. **Right side** remains the default. The file picker retains its
existing layout.

## Two persistent browsing panes

- **F3** or **View → Show Two Panes** splits the browser. On first use, the second pane starts at the current
  location and receives focus. Each pane has its own tabs, breadcrumb, selections, history,
  scrolling, view modes, zoom, sort order, hidden-file setting and filename filters.
- **Tab** switches panes while browsing. Clicking a pane's header focuses it without disturbing
  its selection. Hovering or scrolling the other pane does not move keyboard focus. The active
  header has an accent and an “Active” label; inactive selections remain visible in subdued colors.
- **Ctrl+T**, **Ctrl+W**, **Ctrl+Tab**, **Ctrl+Shift+Tab** and **Alt+1…9** operate in the active
  pane. The last tab in each pane stays open. New tabs inherit view, zoom, sorting and hidden-file
  visibility, and start with an empty filename filter.
- Drag the divider to resize; double-click it to equalize. **View → Arrange Top / Bottom** changes orientation,
  and **View → Swap Panes** exchanges their positions while keeping focus with the same content.
  Column view works inside either pane, just like Grid and Details.
- **Ctrl+F5** copies selected files to the other pane's current folder; **Ctrl+F6** moves them.
  The command palette and **Panes** context submenu show the destination path and direction.
  In expanded Details views, the destination is the breadcrumb's folder, not a selected child.
  Transfers capture both ends when invoked and use the existing conflict handling, sidecar
  support, progress and window-wide undo. Clipboard copy/paste keeps its usual behavior.
- **Open in Other Pane** opens a selected folder in a new tab there. **Show Same Folder in Other
  Pane** opens the current location there. Both focus the destination without replacing its tabs.
  These commands and layout controls are available in the command palette and **Panes** submenu.
- **F3** again shows the active pane full-width, retaining the hidden pane's tabs and selections.
  Cross-pane transfers are unavailable until both panes are visible again. Reopening rummage
  without a folder restores both tab sets, active tabs, focused pane, orientation, divider and
  whether the second pane was hidden. Older single-pane sessions remain readable.
- One sidebar, information panel and transfer status serve the window. The information panel
  follows the active pane; **Ctrl+I** hides it. Each pane also shows its own selection count.
  The file picker stays single-pane. Text fields and dialogs retain their normal Tab handling.

## Filter the current view

The toolbar's Filter field matches filename substrings, ignoring ASCII case, in
columns, grid and details. It always checks the full filename, even when extensions
are hidden: `svg` finds names containing `svg`, including `.svg` extensions.

Queries containing `*`, `?` or `[` use shell-style wildcard matching against the
whole filename, ignoring case: `*.svg` matches SVG files, `icon?.svg` matches a
single character after `icon`, and `icon[0-9].svg` matches a digit. Use `*svg*` to
match anywhere with wildcards. Backslash escapes wildcard characters, such as
`*\[draft\]*` for names containing `[draft]`. These are wildcard patterns, not
regular expressions.

In Miller columns the filter affects only the last column;
ancestor columns remain visible. **Ctrl+Shift+F** focuses it; Enter keeps the filter and
returns focus to files. Escape or Clear removes it. It persists when switching
views/folders and combines with picker type restrictions. New Folder clears the
filter before starting a rename. Use Ctrl+F for recursive search instead.

## Commands and finding files

**Ctrl+K** opens the command palette. Type parts of a command name; letters match
in order, so `nf` can find New Folder. It also offers places and tabs, shows current
shortcuts, and lists recently used commands first when the query is empty.
Only applicable commands appear. Use arrows or Page Up/Down, Enter to run, and
Escape or Ctrl+K to close. Open With opens an application list within the palette.

**Ctrl+F** searches filenames and paths recursively under the current folder.
Names rank by exact match, prefix, substring, then scattered letters, ahead of
matches requiring parent folders. Within each group, shallower paths come first.
Spaces separate terms that must all match, in any order, in both file search and
the command palette. Long result labels keep their highlighted match visible.
Enter reveals a result; Shift+Enter opens its default application. Space inserts a
space while typing. After Up/Down or Page Up/Down moves into the results, Space
opens Quick Look; Shift+Space always inserts a space. This searches paths, not file contents.

Search excludes hidden entries unless shown, does not follow symlink directories
or cross into other mounted filesystems, and stops at 300,000 entries with a
message. Closing the search releases its index.

## Places, bookmarks and devices

Home folders follow XDG user-directory settings. **Ctrl+D** bookmarks the current
folder; Add to Sidebar in the context menu bookmarks selected folders. Dragging
folders onto the BOOKMARKS heading or the end of its list also adds them. A
right-click offers removal and, for local bookmarks, renaming, destination changes
and reordering. rummage shares the GTK bookmarks file and preserves unrelated
entries and labels. See [editing bookmarks](#editing-bookmarks) for the controls.

With udisks2 available, internal partitions and removable disks appear in the
sidebar with their volume label, or a capacity-based name such as **4.0 TB Volume**
when unlabelled, rather than the mount folder's UUID. Unmounted disks appear
without mounting themselves. Click to mount and open, with
authorization requested when required; use the context menu to mount, unmount or eject. Ejecting an external
disk unmounts its partitions and powers it off when supported. Busy or read-only
devices report errors in the status bar. Encrypted-device unlocking is not
implemented. Already mounted volumes can still appear without udisks2.

Hover a mounted drive to see its available and total space. Right-click it and
choose **Properties…** for mount point, filesystem, device, capacity, used and
available space, and whether the filesystem is mounted read-only. Values are
read in the background when opened; no directory tree scan is needed. Available
space can be smaller than capacity minus used space because of filesystem
reservations. Unmounted drives must be mounted before these details are available.

## Network folders

`smb://`, `sftp://`, `ftp://` and `dav://`/`davs://` (WebDAV) bookmarks in the GTK
bookmarks file appear in the sidebar too, dimmed until mounted. A click mounts one
through gvfs on the session bus (no GLib in rummage) and opens its gvfs FUSE folder.
A server asking for credentials gets a card with user, domain, a masked password,
Anonymous where allowed, and Remember the password (stored by gvfs in the keyring).
A mounted share has an eject icon; its context menu mounts, unmounts and removes
the bookmark. Without a running gvfs the status bar says so.

**Ctrl+L** opens a card for one location: a folder (`/data`, `~/Pictures`,
`file://…`) or a server (`smb://server/share`, `sftp://user@host/path`,
`ftp://host`, `davs://host/path`, or a Windows share `\\server\share`). A server
is added to the sidebar unless unticked, then mounted. An invalid address keeps
the card open with the reason. Remote paths are resolved within the server's root.

## Sidebar zoom and properties

Use Ctrl+wheel over the sidebar, or F6 then Ctrl+plus/minus/0, to adjust its zoom
independently of file lists. Its size is remembered across restarts. Status bar
text has its own [configuration setting](configuration.md#settings-reference).

**Alt+Enter** or Properties in the context menu shows metadata for one entry,
or the current folder from the background menu. Metadata is read-only and selectable: drag, double-click a word, triple-click a
line, or Shift+click to extend. **Ctrl+A** selects the focused field and **Ctrl+C**
copies its selection. Long filenames and metadata values wrap in full. Folders
also offer Change Icon; see [folder icons](appearance.md#folder-icons-and-properties).

## Preview or work with files

Press **Space** for Quick Look; use arrows to walk through files and Space/Escape
to close it. See [previews](previews.md) for image/text support and video limits.
See [file operations](file-operations.md) for conflicts, clipboard, drag/drop,
sidecars and undo, or [actions](actions.md) to add external commands.

## Editing bookmarks

Right-click a local bookmark for **Rename**, **Choose Folder**, **Move Up** and **Move Down**.
Rename changes its label; Choose Folder replaces its destination while keeping its label
and position. With keyboard focus in Places, F2 edits the label and Alt+Up/Down moves it.
Enter or a click outside saves the label; Escape cancels. Changes are written to the shared
GTK bookmarks file, preserving unrelated lines and network bookmarks.

**Copy Path** (Ctrl+Shift+C) copies the active pane's current folder, regardless of selection.
It is also available from the background menu and command palette. **Copy Location** continues
to copy selected paths, and **Copy Name** continues to copy filenames.
