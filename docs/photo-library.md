# Photo Library

[Documentation](README.md) · [Installation](install.md#photo-library-add-on) · [Previews](previews.md) · [File operations](file-operations.md)

Photo Library combines photographs from registered folders into one searchable collection.
Originals remain in their existing locations. The optional `rummage-photos` add-on stores
organisation in a local catalog; it needs no account, server, model download or network service.
Video and PDF previews belong to the standard application and have separate Settings switches.

## Add folders and find photographs

Install the matching photo add-on, leave **Enable Photo Library** enabled in General settings,
and choose **Photo Library** in the sidebar or command palette. A folder's context menu also
has **Add to Photo Library…**. Picker and portal windows do not expose the library.

Choose **Add folder**, enter its absolute path, review **Include subfolders**, and optionally
enter source-relative exclusions. Adding a folder starts discovery without copying or rewriting
its contents. Overlapping source roots are refused; files with the same name in different
folders retain separate identities. Symbolic links inside sources are not followed.

The **Library** shows all active sources together while discovery continues. Search matches
stored filenames, descriptions and tags. **Filters** adds minimum rating, capture-date bounds,
**Unknown date only**, and an option to include inactive sources. **Sources** and **Albums**
can narrow the same collection. **Clear filters** returns to the active library. **Sort** cycles
through name, capture date and rating. **Save filter** records the current search for reuse.
Search is indexed text search; there is no visual or semantic search in this implementation.

**Gallery view** and **Details view** share the result set and selected photo IDs. Results are
paged. **Select loaded photos** selects the current loaded page; it does not select every
matching result. Selection can span pages, and the displayed selected-ID count is the action
scope. Clear the selection before beginning an unrelated bulk edit.

Enter or Space opens the focused photo in **Quick Look**, using Rummage's existing
preview. **Show in folder** returns to the original location. An offline original
cannot be opened; cached thumbnails and known information remain available. A missing cached
thumbnail is shown as a placeholder, without discarding the photo's organisation.

## Edit and organise

Select photographs and choose **Edit selected**. The inspector shows the affected-photo count.
Shared fields show their value; differing or unloaded values are explicitly marked as mixed.
Apply only the fields you intend to change. Tags have separate add and remove controls, which
preserve unrelated tags. Changes are reported as saved only after the catalog write succeeds.

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
The inspector identifies an extracted, user-supplied or unknown date. Rescanning preserves
manual annotations and date overrides.

Use **Albums → Create album**, then add or remove the current selection using the album's
controls. Filesystem-browser annotations, xattrs and sidecars retain their existing separate
semantics. The photo library does not silently import or synchronise those editable values,
and it does not rewrite embedded metadata. Explicit external-annotation import is not yet
provided.

## Sources and background work

**Activity** lists discovery and duplicate-review jobs, their states, known progress and file
errors. Pause, resume, cancel or retry the named job. Completed work remains recorded. A pause
prevents new items from starting at a safe checkpoint; already claimed metadata work can finish.

The toolbar's **Pause background work** and **Resume background work** apply across library
windows and persist through restarts. Resuming globally leaves individually paused jobs paused.
Source changes can still queue a refresh while work is paused; processing waits for resume.
Use **Pace: Normal** or **Pace: Gentle** to choose the background workload. Gentle uses smaller,
less frequent batches to leave more resources available for browsing. This preference also
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
failing to register a folder displays a warning. Use **Sources → Refresh folder** for one source
or **Refresh library** for all active sources when automatic monitoring is incomplete, after
external changes not yet reflected in the library, or to recover from a watcher problem.
Refreshing remains available even when a folder cannot be watched; it respects background
pause settings. You can also retry a waiting job after reconnecting its source.

**Relink** reviews a new source location before applying it. Existing filesystem identities must
match; ambiguous replacements block the change. Relinking is conservative and does not treat
matching filenames or equal bytes as sufficient identity. **Remove** hides a source from active
views and stops its work while keeping annotations. **Forget data** requires a removed source
and confirmation, and discards its catalog records. Neither action deletes originals.

Confirmed moves and renames retain organisation when filesystem identity is preserved.
Moves to another filesystem or outside registered sources can require location review;
**Activity → Review outcome** retains the original and destination details. These unresolved
matches are not automatically attached to a different file.

## Review exact duplicates

Choose **Review → Find exact duplicates**. Size and content signatures select candidate groups;
full byte comparison runs again before a trash request. Review complete locations and select
which copies to keep. The proposed first cataloged copy is only a suggestion. It does not mean
that this copy has better quality or metadata.

The confirmation shows the originals to retain and trash. Rummage records the intent before
using its normal trash queue and sidecar rules. Immediately before each original is moved,
the queue checks its captured identity and the retained copies' identities again. Changed or
offline candidates remain in place and require another review. This is ordinary recoverable
trash, with no automatic permanent deletion or change to retention settings.

A confirmed trash receipt allows **Restore selected from trash** in the inspector. Rummage's
normal restore conflict choices apply. Changed receipts are refused. If filesystem completion
and catalog recording were interrupted, reopening the library attempts reconciliation from
existing receipts; it never repeats a deletion blindly. Unresolved results need review and
must not be treated as a completed cleanup. Near-duplicate and burst review are not provided.

## Export and recovery

**Sources → Export annotations** writes a versioned JSON file to a new absolute filename.
Existing files are not overwritten. Exports contain annotations, date overrides, albums,
saved filters, source mappings and identity information. They contain no photograph pixels.
Keep exports somewhere separate from the working catalog.

Use **Restore annotations** with the versioned export, including in a fresh catalog.
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
