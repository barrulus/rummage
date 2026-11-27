<p align="center"><img src="data/rummage.svg" width="96" height="96" alt=""></p>

<h1 align="center">rummage</h1>

<p align="center">
  <b>A native file manager for Wayland.</b><br>
  C++20, Wayland and OpenGL ES. No GTK, Qt or GLib.<br>
  Columns, thumbnails and details, with keyboard navigation and isolated image and text previews.
</p>

<p align="center">
  <img src="docs/images/rummage-dark-grid.webp" alt="rummage in a neutral dark appearance, showing SVG artwork thumbnails and the selected poster's preview and metadata" width="100%">
  <br>
  <sub>Artwork thumbnails, stock desktop icons, and a closer look in the information pane.</sub>
</p>

| Columns with a text preview | Video thumbnails in the grid |
| --- | --- |
| ![Nested folders in the dark column view with a selected text file and its preview](docs/images/rummage-dark-columns.webp) | ![Video thumbnails with filmstrip borders and a selected recording's still preview and metadata](docs/images/rummage-dark-video-grid.webp) |

rummage provides three views of your files, two persistent panes, tabs, recursive
filename search, a command palette, Quick Look, and file operations with conflict handling, trash and undo. It
follows desktop colours and icon themes, with a Noctalia palette integration.

rummage is a fork of [Nordend](https://github.com/pankaspe/nordend-fm), created by
[pankaspe](https://github.com/pankaspe). It retains the upstream MIT license and
copyright. These docs describe rummage's 0.6.1 development sources; include the
source commit when identifying a development build. See [release history](docs/releases.md)
and [upgrading from Nordend](docs/install.md#upgrading-from-nordend).

## Start here

- [Install and build](docs/install.md): requirements, Nix, NixOS, Home Manager, Void and CMake.
- [Use rummage](docs/usage.md): views, navigation, tabs, selection, search and places.
- [Explore Folder Insights](docs/insights.md): file types, Iris, sizes, age and source statistics.
- [Configure rummage](docs/configuration.md): settings, reload behavior and saved state.
- [Use rummage as a file picker](docs/file-picker.md): Open/Save dialogs and portal setup.
- [Troubleshoot](docs/troubleshooting.md): startup, previews, menus and desktop integration.
- [All documentation](docs/README.md): application guides and references.

From this checkout, with Nix flakes enabled:

```sh
nix run . -- ~/Pictures
```

Or build with the [required dependencies](docs/install.md#build-dependencies) installed:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/rummage ~/Pictures
```

Run the GUI in a Linux Wayland session with OpenGL ES 3.0. Start with **Ctrl+1/2/3**
to change views, **Space** for Quick Look, **Ctrl+K** for commands, **Ctrl+F** to find
files, and **F1** for the active shortcuts.

## What it does

| Area | Capabilities |
| --- | --- |
| Browse | Columns, grid, expandable details, dual panes, tabs, filters, sorting and zoom |
| Find | Fuzzy filename/path search under the current folder; command palette with places and tabs |
| Preview | Images including SVG and HEIC/HEIF, camera previews, DNG development and text with syntax colours; optional video thumbnails/stills |
| Understand | Folder Insights: Types, Map, Iris, Largest, Age and Languages |
| Manage | Copy, move, rename, create, trash, restore, permanent delete, undo/redo and darktable sidecar handling |
| Integrate | Desktop applications, Open With, terminals, bookmarks, network shares (gvfs), removable disks, clipboard and drag and drop |
| Customize | Fonts, icons, colours, transparency, shortcuts, external actions and nested context menus |
| Pick files | Standalone selection commands and an optional XDG FileChooser portal backend |

The [feature overview](docs/features.md) links to the behavior and limitations of each
area. Image and text decoders run in separate preview helpers; see the
[security model](docs/security.md) for the boundaries and exceptions.

## Photo Library

The optional `rummage-photos` add-on brings albums, saved searches and photo annotations
into Rummage's existing browser. Add source folders to build your collection; originals
stay in place when you organise them into albums.

![Photo Library showing a photo grid with albums and sources expanded in the sidebar](docs/images/rummage-photo-library.webp)

See the [Photo Library guide](docs/photo-library.md) for a screenshot walkthrough and
[installation and setup](docs/install.md#photo-library-add-on).

## Development downloads

<!-- development-downloads:start -->
[Latest development build](https://github.com/barrulus/rummage/releases/tag/development):
unsigned Linux x86_64 snapshots, updated after all checks pass on `main`.

| Distribution | Download |
| --- | --- |
| Debian 13 | [DEB](https://github.com/barrulus/rummage/releases/download/development/rummage-debian13-amd64.deb) |
| Fedora 45 | [RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-fedora45-x86_64.rpm) |
| Arch Linux | [Package](https://github.com/barrulus/rummage/releases/download/development/rummage-arch-x86_64.pkg.tar.zst) |
| openSUSE Tumbleweed | [RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-tumbleweed-x86_64.rpm) |
| Void glibc | [XBPS bundle](https://github.com/barrulus/rummage/releases/download/development/rummage-void-glibc-x86_64.tar.gz) |

Video and PDF previews are included in the standard package, with switches in
Settings. `rummage-photos` is the only optional add-on; the Void bundle includes it.

Follow the [installation and checksum instructions](https://github.com/barrulus/rummage/blob/main/docs/install.md#development-downloads).
Use the package for your exact distribution. For Nix, use
`nix run github:barrulus/rummage/development`; pin the commit in
[source.json](https://github.com/barrulus/rummage/releases/download/development/source.json)
for a fixed revision. These are individual development packages, not an update repository.
<!-- development-downloads:end -->

## Contribute

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and
[architecture](contributing/architecture.md) for the source map.

If you find rummage useful, you can [support its development on Ko-fi](https://ko-fi.com/barrulus).

## License

[MIT](LICENSE). Bundled code and data are credited in [THIRD_PARTY.md](THIRD_PARTY.md).
