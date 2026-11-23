# Keyboard shortcuts

[Documentation](README.md) · [Using rummage](usage.md) · [Configuration](configuration.md)

These are defaults. **F1** (or `?`) shows the active bindings, including custom
ones. Editors, dialogs, Quick Look and file pickers have their own input context;
a shortcut's meaning follows the focused control.

## Views, navigation and search

| Keys | Action | Configurable ID |
| --- | --- | --- |
| Ctrl+, | Settings | `show_settings` |
| Ctrl+1 / Ctrl+2 / Ctrl+3 | Columns / grid / details | `columns_view`, `grid_view`, `details_view` |
| Ctrl+I | Toggle information pane | `toggle_info` |
| Alt+J / Alt+K | Scroll the information pane down / up by three lines | `scroll_preview_down`, `scroll_preview_up` |
| Alt+Page Down / Alt+Page Up | Scroll the information pane by a page | `scroll_preview_page_down`, `scroll_preview_page_up` |
| Ctrl+B | Toggle sidebar | `toggle_sidebar` |
| F6 | Focus sidebar / return to view | `focus_sidebar` |
| Shift+F6 | Focus the file list; leave path editing or filename filtering | `focus_files` |
| No default | Focus Places | `focus_places` |
| Alt+D | Edit the breadcrumb path, with Tab/Shift+Tab completion | `edit_path` |
| Ctrl+H | Show/hide hidden files | `toggle_hidden` |
| No default | Toggle thumbnails / file-type icons | `toggle_thumbnails` |
| No default | Show/hide file extensions | `toggle_extensions` |
| Ctrl+D | Bookmark current folder | `add_bookmark` |
| Ctrl+K | Command palette | `command_palette` |
| Ctrl+L | Go to a folder or server | `go_to` |
| Ctrl+Shift+F | Filter filenames in the view | `filter_names` |
| Ctrl+F | Find files under current folder | `find_files` |
| Space | Quick Look | `quick_look` |
| F1 or ? | Shortcut overlay | `show_shortcuts` |
| Menu or Shift+F10 | Context menu | `context_menu` |
| Alt+Left / Alt+Right | History back / forward | `back`, `forward` |
| Enter / Alt+Down | Open selected file/folder or focused sidebar location | `open_selected` |
| Backspace or Alt+Up | Parent folder | `parent_folder` |
| Right / Left | Column hierarchy; expand/collapse/parent in details | `move_right`, `move_left` |
| Up / Down, Page Up/Down, Home/End | Move through entries or Places | `move_up`, `move_down`, `page_up`, `page_down`, `first_entry`, `last_entry` |
| Ctrl+plus / Ctrl+minus / Ctrl+wheel | Increase/decrease zoom | Fixed |
| Ctrl+0 | Reset zoom | Fixed |
| Ctrl+Q | Quit | Fixed |

## Panes

| Keys | Action | Configurable ID |
| --- | --- | --- |
| F3 | Toggle two panes / active pane full-width | `toggle_split` |
| Tab | Switch active pane while browsing | `switch_pane` |
| Ctrl+F5 | Copy selection to the other pane | `copy_other_pane` |
| Ctrl+F6 | Move selection to the other pane | `move_other_pane` |
| No default | Swap pane positions | `swap_panes` |
| No default | Change side-by-side / top-bottom orientation | `orient_panes` |
| Ctrl+Alt+Enter | Open selected folder or focused sidebar location in the other pane | `open_other_pane` |
| No default | Focus the first / second pane (left/right or top/bottom) | `focus_first_pane`, `focus_second_pane` |
| No default | Expand / collapse a folder; in Grid, open it / go to parent | `expand_folder`, `collapse_folder` |
| No default | Show current folder in the other pane | `same_other_pane` |

Tab retains its normal focus behavior in text fields and dialogs. The picker stays
single-pane. Cross-pane transfers require both panes to be visible. Tabs and file
commands below operate in the active pane.

## Tabs and selection

| Keys | Action | Configurable ID |
| --- | --- | --- |
| Ctrl+T | New tab | `new_tab` |
| Ctrl+Enter | Open selected folder or focused sidebar location in a new tab | `open_selected_tab` |
| Ctrl+W | Close current tab, except the last | `close_tab` |
| Ctrl+Shift+T | Reopen the last closed tab in this pane | `reopen_tab` |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab | `next_tab`, `previous_tab` |
| Alt+1…9 | Select numbered tab | Fixed |
| Ctrl+A | Select all in the current scope | `select_all` |
| Ctrl+S | Select by filename fragment or wildcard pattern | `select_pattern` |
| Ctrl+Shift+I | Invert selection among visible entries | `invert_selection` |
| Ctrl+Space | Toggle selected item | `toggle_selection` |
| Shift+movement | Extend selection | Fixed |
| Ctrl+movement | Move without selecting | Fixed |
| Escape | Back to one entry/cancel current interaction | Fixed |

Expanded details-tree selections stay within a single parent folder. Mouse
selection gestures are covered in [Using rummage](usage.md#navigate-and-select).

## Files

| Keys | Action | Configurable ID |
| --- | --- | --- |
| Ctrl+Alt+I | Folder insights tab: types, space, largest files, age and languages | `folder_insights` |
| Alt+Enter | Properties / folder icon selection | `properties` |
| F2 | Rename; Batch Rename with several entries selected | `rename` |
| Ctrl+Shift+N | New folder | `new_folder` |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste | `copy`, `cut`, `paste` |
| Ctrl+Alt+C | Copy selected filenames, including extensions | `copy_name` |
| Delete | Trash; permanent delete when already in Trash | `trash` |
| Shift+Delete | Delete permanently with confirmation | `erase` |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo | `undo`, `redo` |
| Shift+Escape | Stop the active transfer, extraction, compression or delete | `stop_transfer` |
| F4 | Open terminal in folder | `open_terminal` |

Copy Location, Extract Here and Extract To have no default shortcuts. Bind their
IDs `copy_location`, `extract_here`, `extract_to` or `action:<id>` explicitly.
Copy Name copies plain text, with multiple names separated by newlines. Its
`copy_name` binding also accepts the alias `action:copy_name`.

## Context-specific keys

| Context | Keys |
| --- | --- |
| Quick Look | Space/Escape closes; arrows change file |
| Text Quick Look | Home/End, Page Up/Down scroll; Ctrl+A selects text; Ctrl+C copies selection; Shift+wheel scrolls horizontally |
| Markdown Quick Look | M switches between rendered text and source |
| Palette/search | Arrows/Page Up/Down choose; Enter runs/reveals; Escape closes |
| File search | Space types a space while editing; after arrow-key navigation it previews; Shift+Space always types a space; Shift+Enter opens in application |
| Rename editor | Enter accepts, Escape cancels, Ctrl+arrows moves words, Ctrl+C / Ctrl+X / Ctrl+V copies/cuts/pastes text, Ctrl+Z / Ctrl+Shift+Z edits undo history |
| Batch Rename | Tab/Shift+Tab moves between fields and controls; Enter applies valid changes; Escape cancels; Page Up/Down scrolls the preview |
| Conflict dialog | R replaces a file or merges folders, S skips, Enter keeps both, A applies to rest, Escape stops |
| Permanent-delete dialog | Enter confirms, Escape cancels |
| Context menu | Up/Down chooses, Right enters a submenu, Left/Escape backs out, Enter runs |
| File picker | Ctrl+Enter accepts; Tab/Shift+Tab cycles controls; Escape closes transient UI before cancelling |

## Override bindings

Use **Settings → Keyboard shortcuts** to edit the **Thumbnails** and **File extensions**
commands (both unbound by default), or add to one `[keybindings]` table in `config.toml`:

```toml
[keybindings]
rename = "Ctrl+r"
select_all = "Ctrl+g"
toggle_selection = "Ctrl+j"
erase = ""
copy_location = "Ctrl+Shift+c"
toggle_thumbnails = "Ctrl+Alt+t"
toggle_extensions = "Ctrl+Alt+e"
```

Each entry replaces all default chords for that action. An empty string disables
it, and an omitted entry retains defaults. Use modifiers `Ctrl`, `Shift`, `Alt`,
`Super` and XKB key names; key names are case-insensitive, modifier spellings are
as shown. Use `Ctrl+plus`, not `Ctrl++`, for a literal plus key.

Custom actions use quoted keys such as `"action:compress" = "Ctrl+Alt+z"`; first
define the action using the [actions guide](actions.md). Numbered-tab, quit and editor/dialog keys remain fixed. Movement bindings retain
Shift for extending selection and Ctrl for moving without changing selection;
choose a base chord without Ctrl or Shift. Conflict detection includes these
selection variants.

Unknown IDs, invalid chords and conflicting bindings reject the binding set.
Actions, menus and bindings reload together; the prior valid set survives an
invalid reload. Run `rummage --check-config` and check F1 after editing.

## Jump to a folder

Use a quoted `go:<path>` key in the existing `[keybindings]` table:

```toml
[keybindings]
"go:~/Downloads" = "Ctrl+Alt+d"
"go:/srv/projects" = "Ctrl+Alt+p"
```

The shortcut opens the folder in the active pane’s current tab and preserves back/forward
history. It works from the views, sidebar and Quick Look; editors and dialogs keep
their input. Missing destinations show a message and leave the current folder in
place. Absolute paths, `~` and `~/` paths are supported, without shell or variable
expansion. Bindings reload live and appear in F1; the usual chord-conflict checks
and empty-string disabling apply.

### Bookmark and preview additions

| Shortcut | Action |
| --- | --- |
| Ctrl+Shift+C | Copy the active folder's path, regardless of selection (`copy_path`) |
| F2 with Places focused | Rename the selected local bookmark |
| Alt+Up / Alt+Down with Places focused | Move the selected local bookmark |
| I in Quick Look | Show or hide Details |
| Ctrl+A / Ctrl+C on a selected Details value | Select all / copy that value |

### Editing shortcuts in Settings

Settings → Keyboard shortcuts has a searchable command list, including custom actions and
configured folder shortcuts. Select a command and choose **Record shortcut**, then press the
combination. Modifier-only keys are ignored; Escape cancels recording. Conflicts are shown
immediately and rejected combinations are not saved. **Reset shortcut to default** removes the
configuration override; **Disable shortcut** writes an explicit empty binding. These are distinct
operations: resetting can restore a built-in shortcut.

Folder shortcuts accept an absolute path or `~/` path before recording. Custom actions can also
receive shortcuts directly from their editing form under Settings → Actions. No action or folder
shortcut executes while it is being edited.

`reset_folder_view` restores the active folder’s default view preferences and has
no default shortcut. It is also available in Sort By and the command palette.

### Path completion and drop choices

| Gesture | Action |
| --- | --- |
| Tab / Shift+Tab while editing the breadcrumb path | Complete a path and cycle matches forward / backward |
| Enter while editing the breadcrumb path | Open the folder or reveal the file |
| Alt while dropping files | Choose a copy, move, grouping or archive extraction action |

### Browse entirely with the keyboard

Use **F6** to focus Places, choose a row with arrows or Page Up/Down, then use
**Enter**, **Ctrl+Enter** or **Ctrl+Alt+Enter** to open it in the current tab, a new
tab or the other pane. **Menu / Shift+F10** opens that row’s context menu with its
first enabled action selected. **F4**, **Alt+Enter** and **Ctrl+Shift+C** act on
that location too, opening a terminal, showing Properties or copying its path.
Unmounted locations must be mounted before path-based actions become available.
Escape from a sidebar menu returns to the same sidebar row; opening a location
hands focus to its file list. **Shift+F6** explicitly returns to the file list.

**Alt+D** selects the breadcrumb path for editing. Enter navigates, while Escape
or Shift+F6 cancels the edit. Filename filters remain applied when Shift+F6 returns
to the files. The active file area gets an outline during keyboard browsing;
Places uses a highlighted focus row. Dialogs and text editors retain their own
movement and text input, and closing them restores the underlying browsing focus.

The new navigation and focus commands also appear in the command palette and the
shortcut editor. With Places focused, the palette offers actions for its focused
location and commands to return to browsing. For a single selected folder, Ctrl+Enter focuses its new tab;
several selected folders open background tabs. Files open through the usual Open
command. In a file picker, Ctrl+Enter continues to confirm the selection.

For example, to move with J/K, change these entries in your existing table:

```toml
[keybindings]
move_down = "j"
move_up = "k"
command_palette = "Ctrl+Shift+p"
```

Here Shift+J/K extends the selection and Ctrl+J/K moves the cursor alone. The
palette is reassigned because Ctrl+K now belongs to movement. Original Up/Down
bindings are replaced; editors and dialogs keep their normal arrow keys. Each
action can be reset separately in Settings. New default shortcuts give way to
existing custom assignments.

### Search results

In Ctrl+F, **Ctrl+Enter** opens the query in the Search Results view. There, Down
moves from the query field to the list. Ctrl/Shift-click and Shift+arrows extend
selection; Ctrl+A selects displayed results while the list has focus. Ctrl+C and
Ctrl+X copy/cut the selected files, Delete trashes them, F2 renames one item, Space
previews, Enter opens and Shift+Enter reveals it in its folder. Ctrl+Z/Ctrl+Shift+Z
undo/redo file operations from the list. Ctrl+S saves a query, Ctrl+Shift+S opens
saved searches, F5 refreshes and Escape closes. Tab visits the query, toolbar and
list. The query field retains its normal text-editing shortcuts.
