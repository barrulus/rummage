# Terminal and shell workflows

[Documentation](README.md) · [Shortcuts](shortcuts.md) · [Configuration](configuration.md)

## Embedded terminal

Press **Shift+F4** or choose **Toggle Terminal** in the command palette to show or
hide the terminal below the file view. Drag its top edge to resize it. Click the
terminal to type there; **Ctrl+`** switches focus between the terminal and files.
The close button hides the panel. Hiding it retains the shell and its jobs;
closing the window ends the terminal session and its foreground/background jobs.
Commands which deliberately start a separate session are independent.

The terminal uses `$SHELL`, falling back to `/bin/sh`, and starts in the current
folder. It supports ANSI colours, UTF-8, cursor/function keys, alternate-screen
programs and terminal resize. While it has focus, ordinary browsing shortcuts go
to the terminal; Shift+F4 and Ctrl+` remain panel controls. Use **Ctrl+Shift+V** to
paste (up to 64 KiB), **Ctrl+Shift+C** to copy the visible screen, and the mouse
wheel or **Shift+PageUp/PageDown** for up to 1,000 lines of scrollback. Mouse text
selection and mouse reporting to terminal applications are not supported.

Bash and zsh synchronize working directories in both directions. Rummage reads
the shell's actual working directory and preserves its existing startup files
and prompt hooks using temporary session files. Browser navigation is queued
until the shell reports a clean prompt with no foreground job or typed draft.
It never sends a directory change into a running command or partially typed
line. Finish the command, or clear the draft and press Enter, to release a
queued change. The most recent browser destination takes precedence. A failed
shell directory change leaves the browser where it is and displays an error.
Shell configurations which replace the installed prompt hooks suspend automatic
synchronization. Other shells work interactively without automatic synchronization.

Each window has one terminal. Switching tabs or the active dual pane updates its
queued destination; directory changes made in the shell affect the active pane.
Hiding the panel pauses synchronization. Archives, Trash, search results and
other virtual locations do not become shell working directories. Remote URLs
are unsupported; mounted filesystems with ordinary local paths are usable. A
visible session keeps its last local directory while a virtual view is active.

**F4** and **Open in Terminal** still launch the configured external terminal.
The file picker's [Navigate in terminal…](file-picker.md#navigate-in-a-terminal)
continues to use its separate external-terminal handoff.

## Navigate with fzf or zoxide

Choose **Navigate with fzf…** from the command palette to search files and folders
below the current directory in the embedded terminal. It requires `fzf` and
`find` on `PATH`; traversal includes hidden entries, does not follow symlinked
directories and stops at 64 levels. Enter opens the selected folder or reveals
the selected file; cancelling leaves the browser in place. The NUL-delimited
handoff preserves spaces, Unicode, quotes and newlines in filenames.

Choose **Jump with zoxide…** and enter a directory query to open the best match
from your zoxide database. Install `zoxide` and initialize/populate its database
in your shell first. Query text is passed as one literal argument. Zoxide's
newline-delimited output cannot safely represent a directory containing a
newline; such results are rejected. Both workflows report missing commands and
leave ordinary browsing available.

## Run a command

Choose **Run Shell Command…**, type shell code, and press **Run**. It executes
interactively in the embedded terminal using `sh -c`. The current directory and
selection are captured when the dialog opens. Selected paths are absolute,
literal positional arguments: `$1`, `$2`, and so on; `"$@"` expands all of them.
With no selection there are no positional arguments. For example:

```sh
printf '%s\n' "$@"
```

Quote positional arguments in your command as you normally would in shell code.
The command text itself is shell code, so pipes, redirects and substitutions are
intentional. Commands run with your normal permissions. Output and `[exit N]`
remain in the panel; Ctrl+C interrupts a foreground command. New workflows wait
for a clean Bash/zsh prompt and reject an existing command or typed draft. The
navigation result applies to the active pane when the command completes.

The palette commands can be bound through `[keybindings]` using
`toggle_terminal`, `focus_terminal`, `fzf_navigate`, `zoxide_jump` and `shell_command`.
The existing `[terminal]` settings configure the external terminal; the embedded
shell uses `$SHELL`.

## Optional build support

Packages enable the embedded panel using Paul Evans' MIT-licensed libvterm
(`vterm >= 0.3` in pkg-config). It fits the existing native renderer without a
GTK/Qt widget dependency. Nix calls this library `libvterm-neovim`; its unrelated
`libvterm` package is a different project. Build with `-DRUMMAGE_TERMINAL=OFF`, or
Nix `withTerminal = false`, to omit the library and helper. External terminals
and picker handoff remain available in that build. The embedded terminal
commands explain when support is unavailable.
