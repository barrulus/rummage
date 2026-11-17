# Command line

[Documentation](README.md) · [File picker](file-picker.md) · [Development tools](../contributing/development.md#diagnostic-tools)

## Browser and diagnostics

```text
rummage [--config FILE] [DIR]
rummage [--config FILE] --check-config
rummage --version
rummage --about
rummage --help
```

| Argument | Effect |
| --- | --- |
| `DIR` | Open one directory; without it, follow `startup.mode` (default: Home or restored tabs/panes) |
| `--config FILE` | Select a settings file; does not relocate state or palette |
| `--check-config` | Validate settings, actions, menus and bindings without a window |
| `--version` | Print the compiled project version |
| `--about` | Print version/commit, build ID, system, desktop, GPU and discovered engines for a report |
| `--help`, `-h` | Print usage |
| `--test-scene` | Open the developer renderer/input test scene |

`--about` probes the GPU with a surfaceless context; failures can leave GPU details
unavailable. Include the **Build** line in bug reports: it identifies the linked
executable even between version bumps, including builds with uncommitted changes
and packages built without Git metadata. It is a content-derived ELF build ID,
not a sequential release number; identical builds retain the same ID.
The command line accepts one positional directory. Use `--` before a path whose
name starts with `-`, for example `rummage -- ./-downloads`.

## Show in Folder

```sh
rummage --reveal ~/Downloads/example.zip
rummage --properties -- ~/Downloads/example.zip ~/Pictures/photo.jpg
rummage --open-folders ~/Downloads ~/Pictures
rummage --set-file-manager
rummage --unset-file-manager
```

`--reveal PATH...` selects each item in its parent folder, grouping items by
folder into tabs and focusing the first folder. Directories are selected in their
parent too. `--properties` also opens the info pane with More details expanded.
`--open-folders DIR...` opens directory tabs and keeps the first one focused.
Paths may be relative; `--` ends option parsing. Missing items and non-directory
folder arguments are skipped; no existing paths gives exit status 1 without a
window. These modes cannot be combined with a picker or service mode.

`--set-file-manager` opts into the desktop's `org.freedesktop.FileManager1`
interface used by browsers' **Show in Folder** actions. It writes only
`$XDG_DATA_HOME/dbus-1/services/org.freedesktop.FileManager1.service` (normally
`~/.local/share/dbus-1/services/`). Another program's service file is left alone.
If another file manager already owns the bus name, close it or log out and in
before rummage answers. `--unset-file-manager` removes only rummage's own service
file; its idle service exits within 30 seconds. Re-run setup after moving a local
build or removing the installation named in the service file.

The service is activated as `rummage --file-manager1`, separately from the
FileChooser portal. ShowFolders, ShowItems and ShowItemProperties launch a new
window per request, forwarding the caller's activation token. Only local `file://`
URIs with an empty host or `localhost` are supported. Remote URIs, malformed
escapes, NUL bytes and query/fragment components are ignored. A valid request's
filesystem checks happen in its child so an unavailable mount cannot block the
service. Compositor policy determines whether the window takes focus.

This choice is independent of the default folder application. To make ordinary
folder opening use rummage too, run `xdg-mime default rummage.desktop inode/directory`.
The setup command reports the current folder association but does not change it.

## Standalone picker

```sh
rummage --pick ~/Pictures
rummage --pick --multiple ~/Pictures
rummage --pick-directory ~
rummage --pick-directory --multiple ~
rummage --save-file --name Untitled.txt --title 'Save notes' ~/Documents
```

Put the mode before its modifiers. `--multiple` is for open modes, not Save File.
`--name NAME` supplies an initial filename and `--title TEXT` a window title.
Selected absolute paths are **NUL-separated on stdout**. A save picker returns a
destination; it does not write data. Do not store NUL output in a shell variable.

For human-readable output only:

```sh
rummage --pick --multiple ~/Pictures | tr '\0' '\n'
```

Newline conversion loses unambiguous filename boundaries and a pipeline's status
may hide cancellation. For scripts, capture output and status separately:

```sh
selection_file=$(mktemp)
if rummage --pick --multiple ~/Pictures > "$selection_file"; then
    python3 - "$selection_file" <<'PY'
import os
import pathlib
import sys
for path in pathlib.Path(sys.argv[1]).read_bytes().split(b'\0'):
    if path:
        print(repr(os.fsdecode(path)))
PY
else
    status=$?
    printf 'Picker exited with status %s\n' "$status" >&2
fi
rm -f -- "$selection_file"
```

This example prints escaped paths; replace that consumer with the work you need.
See [picker interaction](file-picker.md#try-without-changing-desktop-defaults) for
accepting folders, filters, keyboard focus and cancellation.

## Portal mode

`rummage --portal` runs the D-Bus FileChooser backend. Normally the session bus
activates it using the installed service file. Configure backend selection using
the [portal guide](file-picker.md), rather than adding it as an ordinary desktop
autostart command. `--picker-channel` is an internal child-process protocol, not
a public scripting interface. SaveFiles is available through the portal, not as
a standalone multi-save CLI mode.

## Exit status

| Status | Meaning |
| --- | --- |
| `0` | Successful command/normal browser exit; picker accepted |
| `1` | Config validation problems or picker cancellation; runtime failures may also be nonzero |
| `2` | Invalid command-line arguments or malformed internal picker request |

Treat nonzero results as failure to obtain a selection. Check stderr for a
diagnostic rather than assuming every nonzero exit is a user cancellation.

## Environment and signals

| Variable | Use |
| --- | --- |
| `RUMMAGE_ENGINE_PATH` | Colon-separated absolute engine-manifest directories for optional helper installations; restart to discover |
| `RUMMAGE_LOG` | `trace`, `debug`, `info`, `warn`, `error`, `off`; logs go to stderr |
| `RUMMAGE_FORCE_CSD=1` | Request rummage's own title bar; compositor has final say |
| `RUMMAGE_FORCE_XCURSOR=1` | Exercise the Xcursor fallback |
| `XCURSOR_THEME`, `XCURSOR_SIZE` | Cursor fallback theme and size |
| `TERMINAL` | Preferred terminal for terminal actions/application launches |
| `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME` | Standard storage locations |
| `XDG_DATA_DIRS`, `XDG_CURRENT_DESKTOP` | Desktop data/association discovery |
| `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` | Wayland session connection |
| `XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID` | One-shot startup focus token, consumed before child launches |

Release builds log at `info` by default; debug builds at `debug`. SIGUSR1 asks the
browser for a memory report; SIGINT/SIGTERM request orderly shutdown. Startup
benchmark variables and `RUMMAGE_SANDBOX_REPORT` are development instrumentation,
described in [development](../contributing/development.md) and [security](security.md).
