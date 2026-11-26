# Workflow extensions and Git

Rummage supports an opt-in version 1 subprocess contract for commands, context
menu actions and file-status badges. It is independent of implementation language
and does not add GTK, Qt or GLib dependencies. Existing [external actions](actions.md)
and isolated preview-helper engines remain separate and unchanged.

## Enable Git

Add this to your configuration file; Rummage reloads it automatically:

```toml
[extensions]
git = true
enabled = []
# Optional literal argv prefix. Rummage appends the repository root as one argument.
git_tool = ["gitg"]
```

Git is disabled by default. When enabled, grid, details and column icons show:

| Badge | Meaning |
| --- | --- |
| M | Working tree modified, deleted or type changed |
| S | Staged addition, change, deletion or rename |
| SM | Staged and subsequently modified |
| ? | Untracked |
| ! | Merge conflict |
| I | Ignored |

Folders aggregate the states of their descendants; mixed states have combined
letters. Ignored directories receive one `I` badge rather than recursively scanning
their contents. Untracked directories similarly receive one `?` badge. Clean files
have no badge. Nested repositories are inspected independently when opened, and
linked Git worktrees are supported. Submodule dirtiness is not recursively inspected.
A symlink path into a repository uses the physical repository for status lookup.

Git → **View Git diff** and **View staged Git diff** open a scrollable, copyable
text dialog for selected paths, or the whole folder on its background. Filenames
are literal Git pathspecs, so characters such as `*`, `?` and `[]` do not select
other files. These
operations are read-only. External diff programs, text conversion and optional
index writes are disabled; inherited `GIT_*` repository/config redirection variables
are removed. **Open repository in Git tool** launches your configured program;
that program may change the repository in response to its own user interface.
Rummage never implicitly stages, commits, resets or switches branches.

Status runs on workers, polled every two seconds, so changes from other programs
and `.git/index` changes are noticed without relying on directory watcher events.
Repository status is shared between folders, cached for two seconds and bounded to
eight repositories, 2 MiB of output and 32,768 records. Root discovery is cached
for five seconds in at most 128 folders (including folders outside repositories).
Creating/removing a nested repository can therefore take several seconds to appear.
Status processes have a three-second limit; discovery has two seconds. Exceeding a
limit clears that result and reports an error instead of displaying partial status.
Huge or remote repositories may consequently show no badges. Git is invoked once
per repository refresh, not once per file or frame. Outside repositories no badges
are shown. Missing Git and other failures appear in **Extensions → Extension status**.

## Install and manage an extension

Extensions are **trusted code running as your user**, with your filesystem,
network and inherited environment access. They are not preview sandboxes. Only
install programs you trust. A timeout and a process group provide failure cleanup,
not a security boundary; a malicious program can evade process-group containment.
Discovery never executes an unenabled extension.

Copy an extension directory to `$XDG_DATA_HOME/rummage/extensions/ID`, or
`~/.local/share/rummage/extensions/ID` when XDG_DATA_HOME is unset. System locations
are `$XDG_DATA_DIRS/rummage/extensions/ID` (default `/usr/local/share` then `/usr/share`).
The first installed manifest shadows later copies, even if it is invalid. No code
is automatically downloaded or installed by Rummage.

The [bundled example](../data/extensions/example) counts selected files and badges
`.todo` files. It needs Python 3:

```sh
mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/rummage/extensions"
cp -R data/extensions/example "${XDG_DATA_HOME:-$HOME/.local/share}/rummage/extensions/example"
chmod +x "${XDG_DATA_HOME:-$HOME/.local/share}/rummage/extensions/example/helper"
```

Set `enabled = ["example"]` in `[extensions]`. Commands appear in the **Extensions**
context submenu and command palette, subject to their selection scope. Use
**Reload extensions** after changing an installed manifest. **Extension status** lists
loaded IDs and discovery/status errors; command errors use the existing action
error dialog. Remove an ID from `enabled` to disable it; remove its directory to
uninstall it. Configuration reload cancels this window's active request, and the
next status tick cancels old status work and clears old badges. Other windows drop
stale results by configuration generation; an already running command may finish
within its three-second limit. Changing settings does not undo an extension's own
filesystem changes. Archives, Trash and file picker command menus do not run
workflow actions.

## Version 1 contract

Each installation contains `extension.toml` and its executable:

```toml
[extension]
api = 1
id = "example"       # must match the directory and enabled ID
label = "Example"
program = "helper"   # executable file directly in this directory
status = true        # optional, default false

[[command]]
id = "count"
label = "Count selected files"
selection = "many"   # any (default), one, many (one or more), background (zero)
```

IDs/program names use 1–64 ASCII letters, digits, `_` or `-`; labels are nonempty,
control-free UTF-8 strings of at most 128 bytes. There are at most 64 enabled
extensions, 32 commands per extension and 64 KiB per manifest. Unknown API versions,
missing executables, duplicate commands and invalid scopes reject that installation.
Unknown optional fields are ignored; future incompatible contracts require a new
API version. Version 1 intentionally has no in-process ABI, custom UI widgets,
preview replacement, navigation callbacks or persistent background daemons. Those
are future extension points, not implied capabilities.

Each invocation starts a fresh process with the browsed directory as its working
directory. Standard input is one JSON document; standard output must be one JSON
reply. Standard error is discarded. No shell expands arguments. Requests are
captured when a command is chosen, so later selection changes cannot retarget it.
All paths are percent-encoded `file://` URIs, including non-UTF-8 filename bytes.
Do not split paths on whitespace or decode them as shell text.

Command request and reply:

```json
{"version":1,"operation":"command","command":"count","directory":"file:///tmp","files":["file:///tmp/a%20b.todo"]}
{"version":1,"message":"Selected 1 files","error":false}
```

`files` contains the selected targets (empty for background commands). Replies
require `version:1`; `message` is an optional nonempty, control-free string of at
most 1024 bytes, and `error` defaults to false. Exit status must be zero for a
valid reply. Missing message defaults to “Extension completed”.

Status request and reply:

```json
{"version":1,"operation":"status","directory":"file:///tmp","files":["file:///tmp/a%20b.todo"]}
{"version":1,"decorations":{"file:///tmp/a%20b.todo":{"badge":"TODO","label":"To-do file","severity":1}}}
```

Status `files` lists the current filtered folder entries, at most 4096. Reply keys
must exactly identify requested direct children. Omitted entries clear previous
provider badges. Badges are 1–12 bytes; labels are 1–128 bytes; neither may contain
control characters. `severity` is 0 neutral, 1 information, 2 warning or 3 error.
Labels carry the full meaning for accessible descriptions; badges should remain
short and distinguishable without colour. Multiple providers' labels/badges are
combined within bounded lengths. One malformed decoration rejects that provider's
whole reply. Providers cannot decorate paths outside the request.

Requests are limited to 1 MiB; replies to 2 MiB and JSON nesting depth 16. Every
process has a three-second deadline and cancellation checks; status providers
share a six-second budget per folder refresh, after Git. Closing a folder/window
cancels its jobs. Process-group descendants are killed and the direct child reaped
on completion or failure. Processes must not daemonize. Status replies are applied
only while the directory revision/configuration generation still matches. Slow
work and manifest reads run off the UI thread; malformed replies, crashes and
nonzero exits become bounded errors without disabling existing file operations.
