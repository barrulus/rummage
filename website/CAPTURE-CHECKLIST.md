# Screenshot and video capture checklist

Updated **24 November 2026** against the current checkout's documentation and
implementation, including its uncommitted development work. The documented
source version for those captures was **0.6.1 in development**; current sources are
**0.7.0 in development**. Check the actual capture binary:
features present in this checkout may not yet be in the published downloads.

## Screenshot batch reviewed: 24 November 2026

Reviewed 34 supplied PNGs. Imported 30 as lossless WebP, preserving their original
dimensions and verifying identical decoded pixels. The first 24 imports are
1265 × 1037; the five desktop captures are 1904 pixels wide. The original PNGs
remain in the supplied Screenshots directory. The first four takes were
superseded by the later neutral captures. Source filenames, checksums, appearance,
and output filenames are recorded in [the screenshot manifest](data/screenshots.json).

Light/dark labels were assigned from the visible UI, not the capture order.
Columns, Details, Quick Look, and Sidebar were captured light first. Grid,
video previews, Shortcuts, and Transfer were captured dark first. The dimming
behind Quick Look is an overlay, not a mistaken dark-theme label.

Your capture checkmarks below are preserved. They record your capture session;
this review records which scenes were actually found in the supplied batch:

| Scene | Source time(s), light / dark | Published asset(s) |
| --- | --- | --- |
| Columns | 19:19:32 / 19:19:54 | `rummage-{light,dark}-columns.webp` |
| Artwork grid and information pane | 19:21:11 / 19:20:53 | `rummage-{light,dark}-grid.webp` |
| Details with column menu | 19:22:13 / 19:22:33 | `rummage-{light,dark}-details.webp` |
| Video grid and information pane | 19:25:55 / 19:25:34 | `rummage-{light,dark}-video-grid.webp` |
| Quick Look | 19:27:03 / 19:27:20 | `rummage-{light,dark}-quicklook.webp` |
| Shortcuts overlay | 19:29:02 / 19:28:45 | `rummage-{light,dark}-shortcuts.webp` |
| Sidebar and removable device | 19:30:47 / 19:30:59 | `rummage-{light,dark}-sidebar.webp` |
| Transfer progress | 19:32:42 / 19:31:51 | `rummage-{light,dark}-transfer.webp` |
| Insights: count, size, map, scope | 19:35:01, 19:35:14, 19:35:53, 19:38:55; custom palette | `rummage-insights-*.webp` |
| Insights: Iris | 20:38:51; custom dark palette | `rummage-insights-iris.webp` |
| Settings: General, Keyboard shortcuts, Effects | 19:40:07, 19:46:07, 19:46:29; custom palette | `rummage-settings-general.webp`, `rummage-settings-keybindings.webp`, `rummage-effects-preview.webp` |
| File picker | 19:47:59; custom palette | `rummage-file-picker.webp` |

This is enough for the current site. There is no current dual-pane, video-in-columns,
or command-palette capture in this batch. The checked Appearance capture shows
General settings, so it is used and captioned as General. The homepage uses the
neutral dark artwork grid, as does the README. The README's columns
image shows a text preview. The grid/information-pane scenes share captures.
All 25 legacy image files have been retired; replacements are named for what
they actually show. The historical replacement filenames below are retained
only as a record of the original capture plan. Video remains pending.

Standalone images now use dark appearances, including Quick Look, the README,
shortcuts, transfers and the sidebar. Light captures remain in explicit
comparisons with dark first. Folder Insights has a homepage showcase and a
[dedicated guide](../docs/insights.md) with all five supplied Insights captures.
The Iris capture now leads the homepage showcase; its caption and guide explain
that the visible scan is partial and only measured files have area.

### Additional desktop captures

The five later captures are included in the homepage desktop gallery and the
Appearance guide. They show customised setups, including transparency; the
neutral light/dark images remain the primary feature comparisons. Source PNGs
are untouched, and their checksums and dimensions are in the same manifest.

- [x] **niri** — 20:04:18, columns with a JSON preview; `rummage-desktop-niri.webp`.
- [x] **Hyprland** — 19:53:31, details and a mounted folder; `rummage-desktop-hyprland.webp`.
- [x] **Umbriel** — 20:06:12, columns with an archive selected; `rummage-desktop-umbriel.webp`.
- [x] **GNOME** — 20:14:41, grid with blue folder icons; `rummage-desktop-gnome.webp`.
- [x] **KDE Plasma** — 20:16:11, expanded details and an image preview; `rummage-desktop-kde.webp`.

## Scope and priorities

Replace the existing screenshots with current rummage captures using neutral
palettes, stock desktop icons, and opaque backgrounds.

The original collection contained 25 screenshots: 16 `guide-*`, six `shot-*`, and
three `readme-*`. The original plan below grouped these into 20 core captures;
the review above records how the supplied batch was used instead.

Keep that replacement set first. Then capture the newer features: Folder
Insights and Space map, expanded metadata, Properties, clipboard-to-file paste,
view controls, reopened tabs, appearance controls, effects, and translations.
The additional feature captures below do not need light/dark duplicates unless
the comparison itself is the subject.

## Capture setup

- [x] Use the intended current build; record `rummage --about`, its source commit, and whether it includes local changes with the captures.
- [x] Confirm Folder Insights, Reopen Closed Tab, and the current Settings controls exist in that binary before recording their scenes.
- [x] Choose one stock desktop icon theme for the primary screenshots.
- [x] Set up neutral light and dark palettes.
- [x] Set `appearance.background_opacity = 1.0` and compositor window opacity to `1.0`.
- [x] Choose Desktop or Built-in as the palette source for the neutral captures so an existing personal palette file does not override the intended colours.
- [x] Keep Animated background and Selection glow off for the main stills; reserve them for the effects demonstration.
- [x] Set Interface text size so labels remain readable in the final image, and hold it constant across the collection; keep list, grid, and sidebar zoom consistent too.
- [x] Keep the icon theme, window size, font size, folder contents, and selection identical between light/dark comparisons.
- [x] Aim for a 1600 × 1000 application window and save original screenshots as PNGs.
- [x] Capture the application window without surrounding desktop clutter.
- [x] Prepare a demo folder containing photographs, documents, Markdown, a PDF, short videos, and an archive.
- [x] Add nested folders, several file extensions, and deliberately varied file sizes so Folder Insights has meaningful proportions and drill-down results.
- [x] Include an image with useful metadata, a long filename, and a sample code file for the expanded details and wrapping demonstrations.
- [x] Install matching media and PDF helpers for scenes that use video stills, PDF previews, or their extended metadata.
- [x] Give the demo files readable names and use recognisable folder names without private information.
- [x] Reserve alternative icon themes and personal palettes for a separate appearance comparison.

Keep the original PNGs. Website-sized images and WebP exports can be prepared
after the collection is reviewed. Suggested capture names below describe the
new originals; the replacement map refers to existing files in `docs/images/`.

## First priority: the five homepage images

These are priorities within the core checklist below, not additional captures.

- [x] Dual-pane hero: a clean source and destination, independent tabs, and the information pane.
- [x] Dark columns: a clear path through nested folders.
- [x] Dark grid: an orderly collection of photographs.
- [x] Dark details: readable columns and an expanded subfolder.
- [x] Light Quick Look: a strong photograph enlarged over the browser.

## Core guide captures: eight scenes in both appearances

### Columns

Show three levels of nested folders, with the selected item clearly visible.
Replaces `guide-dark-columns.webp`, `guide-light-columns.webp`, and
`shot-columns-dark.webp`.

- [x] Dark: `rummage-dark-columns.png`
- [x] Light: `rummage-light-columns.png`

### Grid

Show an orderly folder of photographs with useful thumbnails and one selection.
Replaces `guide-dark-grid.webp`, `guide-light-grid.webp`, and
`shot-grid-dark.webp`.

- [x] Dark: `rummage-dark-grid.png`
- [x] Light: `rummage-light-grid.png`

### Details

Show readable names, sizes, dates, and an expanded subfolder. Include the current
toolbar Sort By control. Leave the information pane visible if space allows;
wide details tables now scroll horizontally beneath the fixed preview pane.
Replaces `guide-dark-details.webp`, `guide-light-details.webp`, and
`shot-details-light.webp`.

- [x] Dark: `rummage-dark-details.png`
- [x] Light: `rummage-light-details.png`

### Grid with information pane

Select a photograph and show its preview and metadata in the information pane.
Use a readable pane width; its edge can be dragged. Wait for metadata to finish
loading and use a file whose values do not expose private information.
Replaces `guide-dark-grid-info.webp` and `guide-light-grid-info.webp`.

- [x] Dark: `rummage-dark-grid-info.png`
- [x] Light: `rummage-light-grid-info.png`

### Quick Look

Show a strong photograph enlarged over the file browser.
Replaces `guide-dark-quicklook.webp`, `guide-light-quicklook.webp`, and
`shot-quick-look.webp`.

- [x] Dark: `rummage-dark-quicklook.png`
- [x] Light: `rummage-light-quicklook.png`

### Keyboard shortcuts

Show the current **F1** overlay with legible bindings. Use defaults so new
commands such as Folder Insights and Reopen Closed Tab are represented correctly.
Replaces `guide-dark-shortcuts.webp`, `guide-light-shortcuts.webp`, and
`shot-shortcuts.webp`.

- [x] Dark: `rummage-dark-shortcuts.png`
- [x] Light: `rummage-light-shortcuts.png`

### Sidebar

Show Places, a few bookmarks, and an available removable device.
Replaces `guide-dark-sidebar.webp` and `guide-light-sidebar.webp`.

- [x] Dark: `rummage-dark-sidebar.png`
- [x] Light: `rummage-light-sidebar.png`

### Transfer

Capture a real copy in progress with source, destination, and progress visible.
Replaces `guide-dark-transfer.webp` and `guide-light-transfer.webp`.

- [x] Dark: `rummage-dark-transfer.png`
- [x] Light: `rummage-light-transfer.png`

## Four additional core captures

One appearance per scene is sufficient initially.

- [x] **Dual-pane hero** — Photos or another source folder on one side, an organised destination on the other, independent tabs, and the information pane. Save as `rummage-dual-panes.png`; replaces `readme-dual-panes.png`.
- [x] **Video in columns** — Select a video and show its still preview and metadata. Save as `rummage-video-columns.png`; replaces `readme-column-preview.png`.
- [x] **Video grid** — Show several distinct video thumbnails with filmstrip borders. Save as `rummage-video-grid.png`; replaces `readme-video-grid.png`.
- [x] **Command palette** — Open **Ctrl+K**, enter a short query, and show useful matching commands. Save as `rummage-command-palette.png`; replaces `shot-palette.webp`.

## Additional documentation images

### Folder Insights: a new showcase feature

Use **Ctrl+Alt+I**, the Folder insights command, or the information-pane button.
Use a local folder outside Trash and archives. Wait for a complete scan for the
main promotional image. Sizes are logical file sizes, not allocated disk space;
file types are grouped by extension, not detected from contents.

- [x] **File types by count** — Show a useful mix of extensions with counts and percentages. Save as `rummage-insights-types-count.png`.
- [x] **File types by size** — Switch the same completed scan to File size and show the changed proportions. Save as `rummage-insights-types-size.png`.
- [x] **Space map** — Show the treemap with readable folder labels and its accompanying list. Save as `rummage-insights-space-map.png`.
- [x] **Iris** — Show nested folder rings and the file-size list. Imported the 20:38:51 capture as `rummage-insights-iris.webp`; retain the visible partial-result context.
- [ ] **Drill-down** — Enter one folder from the map and show the narrower scope with Back available. Save as `rummage-insights-drilldown.png`.
- [ ] **Matching files** — Select a file type and show its exact matches; record revealing a result in the browser for the video. Save as `rummage-insights-matches.png`.
- [x] **Scope controls** — Show Include subfolders and Hidden files, with the current scope clear. Save as `rummage-insights-scope.png`.

### Metadata and previews

- [ ] **Quick Look Details** — Press **I** on a photo with camera/lens/exposure information; show readable, wrapped values. Save as `rummage-photo-details.png`.
- [ ] **Extended image metadata** — Use a PNG or other suitable image with technical fields and embedded text; show only fields actually present. Save as `rummage-image-metadata.png`.
- [ ] **Video metadata** — Show a video still with duration, codec, resolution, and available audio/stream information. Save as `rummage-video-metadata.png`.
- [ ] **Properties** — Press **Alt+Enter** on an image or video; show the thumbnail and scrollable metadata card with action buttons visible. Save as `rummage-properties.png`.
- [ ] **Folder size** — Select a folder, wait for recursive counting to finish, and show Size and Contains in the information pane. Save as `rummage-folder-size.png`.
- [ ] **Markdown Quick Look** — Show rendered headings, a table, and highlighted code; capture the source toggle separately if useful. Save as `rummage-markdown-preview.png`.
- [ ] **Text information pane** — Show a code/text preview with a short selection, demonstrating readable text and copyable content. Save as `rummage-text-info.png`.
- [ ] **PDF preview and details** — Show the first-page still and available document information, including page count. Save as `rummage-pdf-preview.png`.

### Browsing and file operations

- [ ] **Thumbnails on** — Capture a mixed folder with thumbnails in the current view. Save as `rummage-thumbnails-on.png`.
- [ ] **Thumbnails off** — Use **View → Show → Thumbnails** on the identical folder and selection to show stock file-type icons. Save as `rummage-thumbnails-off.png`.
- [ ] **Filename display** — Demonstrate **View → Show → File extensions** and a selected long filename expanding to show its full name. Save as `rummage-filename-display.png`.
- [ ] **Details controls** — Show optional columns, resizing, or the horizontal scrollbar while the preview pane stays in place. Save as `rummage-details-controls.png`.
- [ ] **Clipboard to file** — Paste an image or plain text into the file view with **Ctrl+V** and show the newly created file with its base name selected for renaming. Save as `rummage-paste-to-file.png`.
- [ ] **Reopen Closed Tab** — Capture the command palette entry; record closing and restoring a tab with **Ctrl+Shift+T** for a short clip. Save as `rummage-reopen-tab.png`.
- [ ] **Batch Rename** — Select several files and show meaningful before/after names. Save as `rummage-batch-rename.png`.
- [ ] **Archive browsing** — Show files inside a supported archive and a member preview. Archive contents are read-only. Save as `rummage-archive-browsing.png`.
- [ ] **Folder icons and bookmarks** — Show a folder icon selected through Properties and the corresponding themed bookmark. Save as `rummage-folder-icons.png`.

### Settings, desktop integration, and languages

- [x] **Settings → Appearance** — Show palette source, separate colour mode, icon theme, opacity, and the applied-palette status. Save as `rummage-settings-appearance.png`.
- [ ] **Interface sizing** — Show the Interface text size control and a readable enlarged interface; distinguish this from per-view zoom. Save as `rummage-interface-size.png`.
- [ ] **Preview placement** — Show the preview below Places in the sidebar, with enough vertical space to prevent fallback to the right. Save as `rummage-sidebar-preview.png`.
- [ ] **Icon-theme comparison** — Capture the same folder with a second stock icon theme. Save as `rummage-alternate-icons.png`.
- [x] **Settings → Keyboard shortcuts** — Show a useful command and its editable binding. Save as `rummage-settings-keybindings.png`.
- [ ] **Settings → Actions** — Show a meaningful custom action and its configuration. Save as `rummage-settings-actions.png`.
- [x] **Effects preview** — Show Settings → Appearance → Effects preview, including Replay copy / Replay move and effect controls. These animate a sample and do not transfer files. Save as `rummage-effects-preview.png`.
- [ ] **Optional ambient effects** — Record Animated background and Selection glow as a separate clip, then return to the neutral capture settings.
- [x] **File picker** — Show an actual application's Open or Save dialog using rummage. Save as `rummage-file-picker.png`.
- [ ] **Terminal navigation from the picker** — Record Navigate in terminal, navigating to a demo folder, then exiting back to the picker with its filters retained.
- [ ] **Show in Folder** — Record revealing a local browser download in rummage after FileManager1 integration has been configured.
- [ ] **Translated interface** — Capture a representative menu or dialog in one of the included translations, then restore the primary capture language. Save as `rummage-translated-interface.png`.

Current translation catalogs are German (`de`), Spanish (`es`), French (`fr`),
Japanese (`ja`), Brazilian Portuguese (`pt_BR`), Russian (`ru`), and Simplified
Chinese (`zh_CN`). Language is selected at startup through the environment, not
an in-app language switch. For example, start a separate run with
`LANGUAGE=fr rummage`. For Japanese or Chinese, verify font coverage first.

## Video: 90-second introduction

Tell one small story: browse a project, inspect its files, then organise a
selection, then understand what is in the folder. The extra time gives Folder
Insights a clear demonstration. Use the caption text as narration or on-screen
text. Keep the smaller workflow additions for separate clips below.

### Recording setup

- [ ] Record at 1920 × 1080 with readable interface text.
- [ ] Use the same neutral, opaque appearance as the primary screenshots.
- [ ] Verify the default shortcuts against **F1** in the capture build.
- [ ] Leave optional ambient effects off in the main introduction; use a separate clip to demonstrate them.
- [ ] Use deliberate pointer movements and pause briefly after each action.
- [ ] Keep the original recording for editing.

### Storyboard

- [ ] **0–6 seconds: introduction.** Begin on a clean, populated folder and hold still briefly. Caption: “Meet rummage. A native file manager for Wayland.”
- [ ] **6–17 seconds: three views.** Navigate through columns, then switch the same folder to grid and details. Pause on each. Caption: “Three views. One familiar workspace.”
- [ ] **17–29 seconds: dual panes.** Open the second pane, select a few files, copy them across, and show the result. Caption: “Keep your source and destination side by side.”
- [ ] **29–42 seconds: previews and metadata.** Press **Space** on a photo and **I** for Details, then show a Markdown preview. Caption: “Take a closer look, right down to the details.”
- [ ] **42–51 seconds: search and commands.** Find a file with **Ctrl+F**, then briefly demonstrate **Ctrl+K**. Caption: “Find files and commands from the keyboard.”
- [ ] **51–68 seconds: Folder Insights.** Open **Ctrl+Alt+I** on the demo folder. Show File types by count, switch to File size, then open Space map and drill into a folder. Let the scan finish, or label any edit that skips the wait. Caption: “See what's in a folder, and where the bytes are.”
- [ ] **68–78 seconds: batch rename and undo.** Show the rename preview, apply it, and demonstrate Undo. Caption: “Preview a batch rename. Undo it when plans change.”
- [ ] **78–85 seconds: appearance.** Show the same folder in neutral light and dark appearances with stock icons. Caption: “Make it feel at home on your desktop.”
- [ ] **85–90 seconds: closing.** Hold a clean dual-pane view, followed by the project address. Caption: “Explore rummage at rummage-fm.com.”

### Short feature clips

Aim for roughly 10–20 seconds per clip, with one clear action and its result.

- [ ] **Clipboard to file** — Paste an image, rename the new file, and show Undo. Optionally repeat with plain text.
- [ ] **Restore a tab** — Close a tab and use **Ctrl+Shift+T** to restore its location and view. This is separate from file-operation Undo.
- [ ] **Icons or thumbnails** — Toggle thumbnails on the same folder, then show filename-extension visibility.
- [ ] **Insights to a file** — Select a type, choose a matching file, and reveal it in the browser.
- [ ] **Copyable metadata** — Resize the information pane or open Quick Look Details, select a value, and copy it.
- [ ] **Effects** — Demonstrate Replay copy / Replay move, optional ambient effects, and reduced-motion behaviour. Label the preview as a simulation.
- [ ] **Picker to terminal and back** — Demonstrate navigating from the file picker without losing the selection constraints.

### Video delivery

- [ ] Export a compressed H.264 MP4 for the homepage.
- [ ] Choose a clean poster frame for the player.
- [ ] If using narration, provide captions and a transcript.
- [ ] Optionally record a separate short clip showing video stills and PDF previews.
- [ ] Label video previews as **stills**; rummage does not currently play videos internally.
- [ ] Describe PDF previews as first-page stills, not a full document viewer.
- [x] Describe Folder Insights as a snapshot of the selected local scope, not a live disk monitor or allocated-space analyser.

## Replacement and publishing checks

- [x] Review the new collection for consistent appearance and current rummage UI.
- [x] Prepare optimised website assets while retaining original captures.
- [x] Replace the homepage images, social preview image, and video poster.
- [x] Replace the three README screenshots and update their descriptions.
- [x] Replace or consolidate all legacy `guide-*` and `shot-*` assets.
- [x] Update image references, dimensions, alt text, and captions to match the new captures.
- [ ] Review website feature copy against the capture build before promoting new development features.
- [ ] If screenshots include installation instructions or filenames, use the current Fedora 45 download names rather than the old Fedora 44 ones.
- [ ] Add the MP4 and configure `demoVideo` in `website/hugo.toml` when ready.
- [x] Run the Hugo build and `python3 website/check.py website/public`.
- [ ] Review desktop and mobile layouts, image readability, and video controls in a browser.

## Source references for this revision

- [Using rummage: Insights, view controls, and tabs](../docs/usage.md)
- [Previews, Properties, and extended metadata](../docs/previews.md)
- [Clipboard paste and other file operations](../docs/file-operations.md)
- [Appearance and interface sizing](../docs/appearance.md)
- [Effects preview and ambient effects](../docs/motion.md)
- [Current keyboard shortcuts](../docs/shortcuts.md)
- [Available translations](../docs/translations.md)
- [File picker and terminal navigation](../docs/file-picker.md)
- [Version and release notes](../docs/releases.md)
