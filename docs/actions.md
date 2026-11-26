# Actions and context menus

[Documentation](README.md) · [Configuration](configuration.md) · [File operations](file-operations.md)

## Define an action

Add `[[actions]]` tables to `~/.config/rummage/config.toml`. Actions appear in the context
menu and command palette (**Ctrl+K**) when their selection and MIME rules match. Programs
are optional dependencies: install the ones your actions use. Missing programs are dimmed
in the context menu and omitted from the palette.

This example compresses one or more selected files or folders. rummage asks for the ZIP
filename and destination, calls **7-Zip directly**, then notifies you when the command exits:

```toml
[[actions]]
id = "compress"
label = "Compress to ZIP…"
argv = ["7z", "a", "-tzip", "-spd", "{destination}", "--", "{files}"]
selection = "many"
placement = "menu"
destination = "file"
destination_name = "Archive.zip"
notify_on_completion = true
```

Use `7zz` instead of `7z` if that is your installed executable. `-spd` treats wildcard
characters in selected filenames literally. `7z a` updates an existing archive; choose a
new filename to create a separate archive. No Python or shell wrapper is needed.

The same picker works with other commands. These examples require **ImageMagick** and
**GNU cp**, respectively:

```toml
[[actions]]
id = "resize-image"
label = "Resize Image to Half Size…"
argv = ["magick", "{file}", "-resize", "50%", "{destination}"]
mime = ["image/*"]
selection = "one"
destination = "file"
destination_name = "resized.png"
notify_on_completion = true

[[actions]]
id = "copy-to"
label = "Copy To…"
argv = ["cp", "--archive", "--", "{files}", "{destination}"]
selection = "many"
destination = "directory"
notify_on_completion = true
```

## Destination dialogs

`destination = "file"` opens rummage's save dialog; `"directory"` selects an existing
folder. `destination_name` is an optional literal suggested filename for the file picker.
The picker starts in the current folder, uses the action's label as its title, and passes
the selected absolute path as `{destination}`. Cancelling launches nothing. The selection
and working directory are captured before the picker opens. rummage does not create the
output: the command does, with its own overwrite behavior. The save dialog confirms an
existing file; commands such as `cp` and `magick` can replace output. External operations
are not part of rummage's Undo history.

Omit `destination` and `{destination}` when the application already supplies its own dialog;
for example, File Roller can use
`argv = ["file-roller", "--default-dir", "{directory}", "--add", "{files}"]`.
A destination picker and `{destination}` must be configured together.

## Arguments and matching

`argv` is an argument array, with no shell interpretation, globbing, or environment/tilde
expansion. Placeholders must occupy whole arguments after the executable; do not add shell
quotes around them. Filenames containing spaces remain single arguments.

| Placeholder | Value |
| --- | --- |
| `{file}` | One absolute selected path; requires `selection = "one"` |
| `{files}` | Each selected absolute path as a separate argument; requires `"one"` or `"many"` |
| `{directory}` | The current folder, also used as the command's working directory |
| `{destination}` | The absolute path chosen by rummage's destination picker |

`selection` defaults to `"one"` (exactly one item). `"many"` allows one or more items;
`"background"` requires none; `"any"` allows any number, including none. Use `{directory}`
or fixed arguments with `"background"`/`"any"`, not `{file}` or `{files}`. Without `mime`,
all file types are allowed, but multi-selection still requires `selection = "many"`.
MIME filters accept exact types and patterns such as `"image/*"`; **every selected item**
must match at least one filter. Folders use `"inode/directory"`.

## Completion notifications

`notify_on_completion = true` enables a desktop notification and status message on success
or failure; it defaults to false. rummage sends notifications itself, without `notify-send`.
The option applies to external actions: exit code 0 means success, other exit codes or
signals mean failure. Cancelling the picker or failing to launch sends no completion notice;
launch errors still appear in the status bar. Keep rummage open to receive notifications.
A desktop notification service is needed for popups; the status message still works without
one. Completion follows the launched process, so a GUI launcher that delegates work and
exits early cannot report when that delegated work finishes.

## Default menu groups

File and folder menus put opening commands first, followed by Cut/Copy/Paste and Rename,
then Create Link, Copy Name/Location, archive operations, and applicable extra actions. Trash and
permanent deletion have their own group. Sort By and Panes stay together near the bottom,
and Properties is last. Separators appear only between groups that contain commands.

Empty-space menus start with New Folder/New File and clipboard commands, then folder tools,
extra actions, view controls and Properties. Copy Location is the single current-folder
path command there. Trash keeps restoration separate from permanent deletion; archive
members put Quick Look before Copy to Extract.

Long menus stay within the window and scroll with the wheel or scrollbar. Up/Down,
Home/End and Page Up/Page Down keep the selected command visible. Right opens a submenu;
Left or Escape returns to its parent. An outside click dismisses without activating the
content behind it.

## Arrange and nest menus

Open **Settings → Menus** to edit the common layout or the Files and folders,
Folder background, Trash and Sidebar overrides. **Add command**, **Separator** and
**Remaining commands** append entries. Select an entry to move it up/down, wrap it in
**Group**, or change its label. **Nest** moves it into the preceding custom group;
**Unnest** moves it out one level. On the selected tree row, Alt+Up/Down reorders,
Alt+Right/Left nests/unnests, and Delete removes it. All controls scroll into view in
small windows.

Valid changes save automatically. Duplicate commands or a second Remaining commands
entry are rejected. **Reset layout** removes only the current context override; it
restores the common layout, or the built-in layout when there is no common override.
Removing every entry creates an explicitly hidden menu. **Preview** shows a representative
structure; the running menu filters commands by selection and fills dynamic application
and template submenus at use time. Menu edits never execute a command.

Actions default to the **Actions** submenu. `placement = "menu"` puts an action directly
above that submenu. A `[context_menu]` layout can instead arrange built-in commands and
custom actions into nested groups, in the order you choose. For the three actions above:

```toml
[context_menu]
items = [
  { id = "open" },
  { id = "open_with" },
  { id = "action:compress" },
  { label = "My Actions", items = [
    { label = "Images", items = [ { id = "action:resize-image" } ] },
    { label = "Files", items = [ { id = "action:copy-to" } ] },
  ] },
  { separator = true },
  { remaining = true },
]
```

Use `action:<id>` for actions and built-in IDs such as `copy`, `rename`, `sort`, and
`open_with` for other commands. A group has `label` and nested `items`; groups can nest
further. Add `label` to an ID reference to rename its menu entry, or `separator = true`
to place a line above it. Inapplicable items and empty groups disappear.

`{ remaining = true }` keeps unlisted items in their default grouping. Use it at most once
per layout, at any depth; omit it to hide unlisted items. Referencing `open_with`, `new_file`,
`sort`, or `actions` keeps the parent's remaining children, including dynamically discovered
applications/templates. Moving a child elsewhere removes it from the original group. Each
ID can appear only once per layout. Explicit layout references override action placement;
menu customization does not change command-palette or shortcut availability.

| Layout | Context |
| --- | --- |
| `items` | Optional common fallback for contexts without a specific override |
| `entry` | Files or folders targeted in an ordinary folder |
| `background` | Empty space in an ordinary folder, without a target |
| `trash` | Trash entries or empty space in Trash |
| `sidebar` | Places, devices, bookmarks, and the Trash sidebar item |

Specific arrays override `items`. Without either, rummage uses that context's built-in menu;
an empty array hides its menu. The [example config](config.example.toml) includes the
**complete default layouts for all four contexts**, their separators and submenu contents,
as well as the available IDs and additional custom-action examples. Default reference arrays
and the custom `items` example are alternatives: keep only the overrides you intend to use
and only one `[context_menu]` table.

For upgrade compatibility, sidebar arrays that exactly match either older shipped
default are treated as if they start with `{ remaining = true }`. This restores new
Places and bookmark actions without rewriting the config. Changed ordering,
labels, separators, groups or item lists remain custom layouts; empty arrays still
hide the menu. Include `remaining` in your own layout to retain new actions too.

Copy Name, Copy Location, Extract Here, and Extract To are built-in actions.
Copy Name (**Ctrl+Alt+C**) copies each selected filename, including its extension, as
plain text; multiple names are separated by newlines. It also works in Trash,
using the displayed names, and in file pickers. Copy Location copies absolute
selected paths, or the current folder path when nothing is selected. Extraction is filtered to
applicable archive selections. Missing tools show setup guidance when invoked.
Supported backends include unrar, 7-Zip, File Roller, unzip, gzip and tar. Copy Name and Copy Location cannot be replaced. Override an extraction preset by its ID to change
its label, placement, or enabled state; providing `argv` makes it an external command.
Copy Name is the only registry action available in Trash. External actions are unavailable
in file-picker mode.

Action IDs can also be bound in the existing `[keybindings]` table, for example
`"action:compress" = "Ctrl+Alt+z"`; define the action before enabling its binding. Actions,
keybindings, and menu layouts reload together. Invalid changes retain the previous valid
set on reload. Validate edits with `rummage --check-config` or
`rummage --config /path/to/config.toml --check-config`.

The maintained reference is [config.example.toml](config.example.toml) in this repository.
It is also installed at `share/rummage/config.example.toml`. Updating rummage does not replace
your personal configuration; compare it with the reference when enabling new features.

## Browse and create archives

ZIP, TAR, tar.gz and single gzip files open as folders with **Enter**. Their contents
are read-only: navigate folders normally, press **Space** for Quick Look, or select
members and use **Ctrl+C**, then **Ctrl+V** in an ordinary folder to extract them.
Copy to the other pane works too. An archive's own Quick Look card shows its file
counts, unpacked size and first entries. Enter on a member extracts a private copy
under `$XDG_CACHE_HOME/rummage/opened` and opens its associated application; changes
to that copy do not update the archive. Old copies are cleaned after a day.

The **Compress to ZIP…** and **Compress to tar.gz…** menu and command-palette entries
ask for a filename, then create an archive beside the selection. These built-in
writers need no external archive program. Enter confirms and Escape cancels the
name dialog; existing names get numbered siblings. Progress appears in the status
bar, **Shift+Escape** cancels, and **Ctrl+Z** moves the completed archive to Trash.

Archive browsing supports stored/Deflate ZIP, TAR and gzip. Encrypted members and
other compression methods can be listed but cannot be previewed or copied through
the browser; use the extraction actions below for password-protected ZIP/7z/RAR and
additional formats. Disc-image mounting and editing an archive in place are not
supported. File pickers do not return virtual archive-member paths.

Member extraction stages its output before publishing and never overwrites existing
files. Unsafe paths and unsupported member types are left out with an explanation;
symbolic and hard links must remain within the extracted selection. Large expansion
ratios require confirmation. Archive preview reads are limited to 256 MiB per member;
listings and folder trees also have bounds, so excessive archives report a limit.
Copied-out members participate in file-operation undo; external extraction remains
outside that history.

## Built-in extraction behavior

The automatic presets prefer unrar for RAR, then 7zz/7z for ZIP/RAR/7z. Other
choices include File Roller, unzip for ZIP, gzip for single gzip files and tar
for tar archives, with 7zz/7z as fallbacks. All these programs are optional. Compressed tar is unpacked in two passes
when using 7-Zip without tar.

Command-line extraction creates a new archive-named directory, or a decompressed file
for a single compressed input. Existing output is retained and the new output gets a
numbered sibling. Sources remain in place. Extract To opens rummage's folder picker;
File Roller supplies its own dialogs when it is the selected backend.

Keep rummage open until command-line extraction finishes. Failure or interruption
can leave partial output in a visible `(extracting)` directory. The 7zz/7z and
unrar backends prompt for passwords, including encrypted headers;
cancelling leaves no output. Other CLI backends do not prompt; use Open With for
those encrypted formats. Failures remain in a scrollable dialog with Copy Details,
diagnostics and any retained output location. Extraction is not added to the file-operation undo history.

## Action fields

| Key | Default | Meaning |
| --- | --- | --- |
| `id` | Required for each table | Unique letters, digits, `_` or `-`; also identifies a preset override |
| `label` | Required for a new action | Visible command name |
| `argv` | Required for a new external action | Executable and literal arguments; placeholders are whole arguments |
| `selection` | `"one"` | `one`, `many`, `background` or `any` |
| `mime` | No filter | Exact types or `type/*`; every selected item must match |
| `placement` | `"actions"` for custom actions | `actions` submenu or top-level `menu`; layouts can override this |
| `execution` | `"handoff"` | The supported external execution mode |
| `enabled` | `true` | Disable a definition without deleting it |
| `destination` | `"none"` | `file` for a save path, `directory` for an existing folder |
| `destination_name` | Empty | Literal suggested filename for `destination = "file"` |
| `notify_on_completion` | `false` | Status message and desktop notification when the launched process exits |

Preset overrides retain omitted preset fields. Copy Name and Copy Location cannot be replaced.
See the [annotated reference](config.example.toml) for all default context layouts
and menu IDs. See [troubleshooting](troubleshooting.md#actions-or-menus-are-missing)
when an action does not appear.

### Editing actions in Settings

Settings → Actions lists custom actions separately from their editing form. Choose an action or
**Add action**, then enter a stable ID, a menu label and a program. Each argument has its own text
field and remove button. Values are literal: spaces, quotes and shell-looking text are passed to
the program unchanged. Editing never runs the program.

Use an entire argument for `{file}`, `{files}`, `{directory}` or `{destination}`. The form explains
these placeholders and lets you cycle selection requirements and the destination picker. A
`{destination}` argument requires a file or folder picker; `{file}` requires one selected item and
`{files}` requires one or more selected items. Invalid combinations remain visible with their
validation message and are not saved.

Valid edits save when leaving a field or pressing Enter. A new action remains a draft until its
required fields and rules are valid. Existing IDs stay fixed so menu references and shortcuts
remain intact. Settings preserves advanced action properties and unrelated configuration keys.
Use **Record shortcut** in the action form to assign a shortcut directly. Read-only configurations
allow inspecting and copying values; change their declared configuration to edit them.

The `create_link` menu ID creates symbolic links for the selected entries.
`reset_folder_view` is the reset action inside Sort By; both can be placed in a
custom menu layout.

The `move_into_folder` menu ID moves the selection into a newly named folder.
Sidebar layouts can include `open`, `open_tab`, `open_other_pane`, `open_terminal`,
`copy_path` and `properties`, alongside bookmark and device actions. These actions
use the location that was right-clicked. Availability depends on the location and
whether it is mounted.

### Advanced 7z creation and integrity checks

The selection context menu's **Archives → Advanced archive options** area leaves the simple
ZIP and tar.gz commands unchanged. **Create 7z…** uses installed `7zz` or `7z`;
missing dependencies are reported before asking for a name. Choose Store, Fast,
Normal or Maximum compression, enable password protection and optionally filename
encryption, and set a volume size in MiB (zero means a single archive). Options
apply to the current window and are not saved to configuration. Filename encryption
is available only with password protection. **Verify after creation** is on by
default. **Test archive integrity…** tests one selected archive; enable Password
protection first to supply a password for an encrypted archive.

Passwords are masked, sent through a private stdin descriptor, never added to
command arguments, environment, configuration or diagnostic messages, and cleared
when the job consumes them. Creation uses a private staging directory. A failed,
failed-verification or cancelled job publishes no archive. Cancel kills and reaps
the backend before removing staging. Split volumes are published together inside
a new folder named after the archive plus ` volumes`; keep all volumes together.
Existing files and folders are retained using numbered names. Compression uses at
most two backend threads; progress is indeterminate during the external command.
No command updates an existing archive. Editing archives remains out of scope.
