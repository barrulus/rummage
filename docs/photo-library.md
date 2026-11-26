# Photo Library

[Documentation](README.md) · [Installation](install.md#photo-library-add-on) · [Previews](previews.md) · [File operations](file-operations.md)

Photo Library combines photographs from registered folders into one searchable collection.
Originals remain in their existing locations. The optional `rummage-photos` add-on stores
organisation in a local catalog; it needs no account, server, model download or network service.
Video and PDF previews belong to the standard application and have separate Settings switches.

## Add folders and find photographs

Install the matching photo add-on, leave **Enable Photo Library** enabled in General settings,
and choose **Photo Library** in the sidebar or command palette. A folder's context menu also
has **Add to Photo Library…**. Photo Library opens in a normal browser tab alongside your
folder tabs, with the same sidebar, navigation, view controls and status bar. Picker and portal
windows do not expose the library.

Open the toolbar’s **Photo Library** menu and choose **Add folder…**. Select the folder in
Rummage’s own folder picker, then review **Include subfolders** and optional source-relative
exclusions. The review shows the chosen location; no path typing is required. Adding a folder
starts discovery without copying or rewriting
its contents. Overlapping source roots are refused; files with the same name in different
folders retain separate identities. Symbolic links inside sources are not followed.

The **Library** shows all active sources together while discovery continues. The browser’s
**Search photos…** field matches stored filenames, descriptions and tags. In the Photo Library
menu, **Filter photos…** adds minimum rating, capture-date bounds, **Unknown date only**, and
an option to include inactive sources. **Sources** and **Albums** can narrow the same collection.
**Clear photo filters** returns to the active library. **Save photo search…** records the current
query and filters for reuse under **Saved searches**. Search is indexed text search; there is
no visual or semantic search.

Use the browser’s existing **Grid**, **Columns** or **Details** view. They share the result set
and selected photo identities. The ordinary sort controls order results by name, size,
modification time or original location; Details includes the location so same-name photographs
from different folders remain distinguishable. The information pane shows the focused photo’s
preview, cached metadata and catalog annotations.

Results are paged. The Photo Library menu provides **Previous photo page**, **Next photo page**,
and **Select loaded photos (this page)**. Selecting a page does not select every matching result.
Selection can span pages; the status bar reports its scope. Use **Clear all photo selection**
before beginning an unrelated bulk edit.

Enter or Space opens the focused photo in **Quick Look**, using Rummage's existing
preview. **Show in Folder** opens the original location in a folder tab. An offline original
cannot be opened; cached thumbnails and known information remain available. A missing cached
thumbnail is shown as a placeholder, without discarding the photo's organisation.

## Edit and organise

Select photographs and open **Properties**, or choose **Edit photo annotations…** from the
Photo Library menu or photo context menu. This uses Rummage’s normal Properties dialog and
shows the affected-photo count. Shared fields show their value; differing or unloaded values
are marked **Mixed values**. Editing a field checks its **Apply** switch. Check a blank field
explicitly to clear it across the selection; leave unchecked fields unchanged. **Tags to add**
and **Tags to remove** accept comma-separated tags and preserve unrelated tags. The dialog’s
**Apply** saves the checked changes, while **Cancel** discards the draft. Failed writes leave
the draft available to correct or retry. Catalog annotations remain editable while originals
are offline.

The catalog owns these edits:

| Field | Source and editing policy |
| --- | --- |
| Description | Your catalog text; external descriptions are not imported automatically |
| Tags | Your catalog tags; adding/removing tags does not rewrite unrelated tags |
| Rating | Your catalog rating from 0 to 5 |
| Capture date | Extracted image date, with an optional catalog override; clearing the override restores the extracted value |
| Dimensions and camera | Derived through the existing isolated image engine |
| Albums | Catalog memberships; adding or removing membership never moves an original |

Capture dates accept `YYYY-MM-DD` or a date and time such as `2024-06-01T12:30:00`.
An unknown timezone stays unknown; modification time is not substituted as the capture date.
The information pane and Properties identify an extracted, user-supplied or unknown date. Rescanning preserves
manual annotations and date overrides.

Select photos and use **Create album…** in the Photo Library menu to create an album containing
that selection. The dialog shows how many photos will be included; with no selection it creates
an empty album. Open **Albums** and choose the album to see its photos. The breadcrumb names the
album. Choose **All photos**, click **Photo Library** in the sidebar, or use Back or Parent to
return to the full active library. An album’s context
menu offers **Add selected photos to album** and **Remove selected photos from album**. Filesystem-browser annotations, xattrs and sidecars retain their existing separate
semantics. The photo library does not silently import or synchronise those editable values,
and it does not rewrite embedded metadata. Explicit external-annotation import is not yet
provided.

## Sources and background work

**Activity** lists discovery and duplicate-review jobs, their states, known progress and file
errors in the existing Details view. A job’s context menu offers **Pause job**, **Resume job**,
**Cancel job** and **Retry job** when appropriate. Completed work remains recorded. A pause
prevents new items from starting at a safe checkpoint; already claimed metadata work can finish.

The Photo Library menu’s **Pause background work** and **Resume background work** apply across library
windows and persist through restarts. Resuming globally leaves individually paused jobs paused.
Source changes can still queue a refresh while work is paused; processing waits for resume.
Check **Gentle background pace** in the same menu for smaller, less frequent batches that
leave more resources available for browsing. Uncheck it to restore the normal pace. This preference also
persists across windows and restarts.

| Pace | Discovery entries per batch | Metadata items per batch | Delay before the next batch |
| --- | --- | --- | --- |
| Normal | Up to 32 | Up to 4 | 250 ms |
| Gentle | Up to 8 | Up to 1 | 1 second |

These are scheduling bounds, not processing-speed estimates. A single file can take longer to
read or decode, and currently claimed work can finish after a pause.

Previously used library work resumes on a normal browser launch when the saved workspace
and catalog are present, subject to saved pause settings; opening Photo Library also resumes
eligible work. Picker, portal and desktop Show in Folder entry points do not resume photo work.
Closing its workspace leaves work running while that browser window is alive; closing the final
window ends processing. There is no background daemon.

While the library is active, Rummage monitors registered source folders and discovered
subfolders using its existing directory watcher. The catalog retains the discovered folder
list for the next launch and queues one reconciliation per source on restart to catch changes
made while Rummage was closed. Nearby filesystem events are collected for 150 ms before
queuing a source refresh. Watcher overflow also queues reconciliation; unchanged signatures retain
derived metadata and duplicate fingerprints. Source availability is checked every 30 seconds,
and a reconnected source queues a refresh. An unavailable source is marked offline; failure
to read a drive is not treated as mass deletion.

Automatic monitoring is limited to 4,096 folders per browser window. Reaching this limit or
failing to register a folder displays a warning. Use **Refresh folder** in a source’s context menu
or **Refresh Photo Library** for all active sources when automatic monitoring is incomplete, after
external changes not yet reflected in the library, or to recover from a watcher problem.
Refreshing remains available even when a folder cannot be watched; it respects background
pause settings. You can also retry a waiting job after reconnecting its source.

**Relink source…** in a source’s context menu opens the native folder picker, then reviews
the chosen location before applying it. Existing filesystem identities must
match; ambiguous replacements block the change. Relinking is conservative and does not treat
matching filenames or equal bytes as sufficient identity. **Make source inactive** hides a source from active
views and stops its work while keeping annotations. **Forget source data…** requires an inactive
source and confirmation, and discards its catalog records. Neither action deletes originals.

Confirmed moves and renames retain organisation when filesystem identity is preserved.
Moves to another filesystem or outside registered sources can require location review;
**Review operation outcome** in Activity’s context menu retains the original and destination details. These unresolved
matches are not automatically attached to a different file.

## Review exact duplicates

Choose **Review duplicates** in the Photo Library menu, then **Find exact duplicate candidates**.
Size and content signatures select candidate groups; full byte comparison runs again before a
trash request. Open a group with **Review these copies**. Its photos use the same browser views,
location column and information pane as the library. Use **Keep selected copies** and
**Mark selected copies for trash** in the context menu to choose which originals to retain.
**Move marked copies to Trash…** opens the final review. The proposed first cataloged copy is only a suggestion. It does not mean
that this copy has better quality or metadata.

The browser review shows the originals and their keep or trash decisions. The final confirmation
shows the exact counts to retain and trash. Rummage records the intent before
using its normal trash queue and sidecar rules. Immediately before each original is moved,
the queue checks its captured identity and the retained copies' identities again. Changed or
offline candidates remain in place and require another review. This is ordinary recoverable
trash, with no automatic permanent deletion or change to retention settings.

A confirmed trash receipt allows **Restore selected originals from trash** in the photo context menu. Rummage's
normal restore conflict choices apply. Changed receipts are refused. If filesystem completion
and catalog recording were interrupted, reopening the library attempts reconciliation from
existing receipts; it never repeats a deletion blindly. Unresolved results need review and
must not be treated as a completed cleanup. Near-duplicate and burst review are not provided.

## Export and recovery

**Export photo annotations…** in the Photo Library menu opens Rummage’s native Save picker
with a suggested JSON filename. Saving over an existing export uses the picker’s replacement
confirmation; the completed export replaces the file atomically. Exports contain annotations,
date overrides, albums,
saved filters, source mappings and identity information. They contain no photograph pixels.
Keep exports somewhere separate from the working catalog.

Use **Restore photo annotations…** in the same menu and choose the versioned JSON export in
Rummage’s native Open picker, including in a fresh catalog. Cancelling either picker leaves
the catalog and export files unchanged.
Restoration registers the exported source folders without starting an automatic scan and
restores only established identity matches. The result reports restored, missing and ambiguous
entries; files with the same name are not silently matched. Originals copied onto a different filesystem can have different identities and
require review. An annotation export is not a backup of your photographs or thumbnail cache.

The catalog is normally `~/.local/share/rummage/photos/catalog.sqlite3`, or under
`$XDG_DATA_HOME/rummage/photos/`. Disabling or uninstalling the add-on preserves that directory.
For a filesystem backup, close all Rummage windows and retain the complete directory, including
any SQLite journal files. Schema initialization/upgrades are versioned; failures preserve the
existing catalog and report an error. Do not delete a catalog to dismiss an upgrade error.

This implementation supports still-image cataloging through the available image engines.
Unsupported or damaged files appear as problems without stopping healthy photographs. AI,
people recognition, video cataloging, remote synchronization and automatic folder relocation
are not included.
