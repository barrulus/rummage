# Git status

[Documentation](README.md) · [Configuration](configuration.md) · [Installation](install.md#optional-git-status)

In a build with git support, rummage shows the state of the git repository a folder
belongs to: the branch and the changes in the status bar and the information pane, and
a badge on each changed file. The context menu and the information pane offer the
everyday commands: stage, commit, discard, ignore, fetch, pull and push. Outside a
repository nothing is shown and no git command runs.

## Status bar

The right part of the status bar, before the view buttons, shows:

- the branch in the accent colour, or `Detached at a1b2c3d` for a detached HEAD, with a
  check mark when nothing has changed;
- `↑2 ↓1`: commits ahead of and behind the upstream, when one is set. rummage does
  not fetch, so these numbers are as of your last fetch;
- an operation in progress: merging, rebasing, cherry-picking, reverting or bisecting;
- the number of conflicts, staged, changed and untracked files;
- what the current file's badge means, for example *Modified, partly staged*.

With the pointer on that part, the bar shows the last commit instead of the folder
status: abbreviated hash, subject, author and age.

## File badges

| Badge | Meaning |
| --- | --- |
| `M` | Modified |
| `A` | Added (staged) |
| `D` | Deleted |
| `R`, `C` | Renamed, copied |
| `T` | Type changed (for example a file became a symbolic link) |
| `U` | Untracked |
| `!` | Conflict |
| `•` | A folder with changes inside; it takes the colour of the strongest one |
| faded | Ignored by `.gitignore` |

A bold letter means the change is entirely staged; a regular one still has unstaged
changes. The status bar spells out which, for the current file. Columns and the details
view draw badges at the end of the name, the grid at the top right of the icon. Names of
changed entries take the badge's colour.

**Sort By › Status** (also in the command palette) puts conflicts first, then changes,
new and untracked files, unchanged entries and ignored ones; folders by the changes
inside them. The order follows the status as it changes.

## Information pane

At the end of the information pane, a **GIT** section (folded by its heading) shows the
branch, the upstream with ahead/behind, an operation in progress, the changes and the last
commit with its author and age. Its buttons:

| Button | What it does |
| --- | --- |
| Commit… | Asks for a message and commits what is staged. With nothing staged, it asks first whether to commit every change, new files included |
| Stage All / Unstage All | `git add -A` / takes everything out of the index |
| Fetch, Pull, Push | `git fetch`, `git pull --no-edit`, `git push`; a branch without an upstream is pushed with `-u origin HEAD` |

The same commands are in the context menu's **Git** submenu on a folder's background.

## Context menu

With files or folders selected, **Git** offers:

| Item | What it does |
| --- | --- |
| Stage / Unstage | Adds the selection to the index, or takes it out |
| Commit… | Commits only the selection (new files included), with a message you enter |
| Discard Changes… | After asking: changed files go back to the last commit, new files go to the trash (undo brings them back) |
| Add to .gitignore | Appends `/path` (`/folder/` for a folder) to the repository's `.gitignore`, once |

Items that do not apply to the selection are dimmed. One command runs at a time per
repository; the pane shows which, and the status is read again when it ends. When a
command fails, the status bar shows git's own last lines.

git never waits for input: there is no terminal prompt, SSH runs in batch mode and no
editor opens. Fetch, pull and push therefore need an ssh-agent or a credential helper
for repositories that ask for a login; without one they fail with git's message. They
stop after 120 seconds.

## Updates

rummage reads the status again when a folder on screen changes, and when git changes
the index, `HEAD`, a branch, the reflog, `packed-refs` or `FETCH_HEAD` (a commit, `git add`, a
checkout, a fetch, a merge). Changes are gathered for 300 ms; changes that never stop,
such as a running build, are read at least every 2 seconds. Git runs in the
background with `--no-optional-locks`, so it never takes the index lock from your own
git commands, and the window never waits for it.

A file changed in a folder that is not on screen (and not through git) is seen with
the next change rummage notices. If `git status` takes longer than 5 seconds, it is
stopped and the status bar shows *Git: paused* until you open another folder.

Linked worktrees and submodules are separate repositories: inside a submodule rummage
shows the submodule's state; its parent shows the submodule as one changed entry.
Discovery stops at file-system boundaries, as git's does. A repository git refuses to
read (for example one owned by another user, see `safe.directory`) shows nothing.

## Settings

| Key | Default | Accepted value / behavior |
| --- | --- | --- |
| `git.enabled` | `true` | Show the git status; live |
| `git.ignored` | `"dim"` | Ignored files: `"show"` (plainly), `"dim"`, or `"hide"`; live |
| `git.folder_status` | `true` | Folders mark the changes inside them; live |

The same settings are in **Settings** (**Ctrl+,**). With `enabled = false` the menu, the
information pane section and the sort order are gone too. Hidden ignored files reappear when
you set `ignored` back to `"show"` or `"dim"`.

## Requirements

rummage runs the `git` program found in the `PATH`; no git library is linked. Without
git the feature stays out of sight. Source builds enable it with `-DRUMMAGE_GIT=ON`
(default OFF); see [installation](install.md#optional-git-status). The file picker does
not show git status.
