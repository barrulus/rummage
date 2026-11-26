# Installation

[Documentation](README.md) · [First steps](usage.md) · [Troubleshooting](troubleshooting.md)

Use a development download for your distribution, Nix for a dependency-managed
build, or CMake for a native source build. Fork packages use this repository's
source, not an upstream Nordend release tarball.

## Runtime requirements

rummage needs Linux, a Wayland compositor with xdg-shell, and an EGL/OpenGL ES 3.0
driver. Its preview sandbox supports x86-64 and AArch64. X11-only sessions are not
supported. Install fonts and the shared-mime-info database for text and file types.

A session D-Bus connection provides desktop integration. Optional services and
programs enable particular features:

| Dependency | Used for |
| --- | --- |
| udisks2 and the desktop's authorization setup | Mounting, unmounting and ejecting devices |
| xdg-desktop-portal and a suitable settings backend | Desktop light/dark and accent settings |
| xdg-desktop-portal with rummage selected for FileChooser | Application Open/Save dialogs |
| A desktop notification service | Custom-action completion popups |
| A terminal emulator or `xdg-terminal-exec` | F4 and terminal desktop applications |
| `fzf`, `find`, `zoxide` | Optional [shell navigation workflows](terminal.md#navigate-with-fzf-or-zoxide) |
| unrar, 7zz/7z, File Roller, unzip, gzip or tar | Archive extraction, depending on format |
| Programs named in your custom actions | Those actions only |

FFmpeg libraries are used only by the isolated media helper bundled with the package. The
core application does not require FFmpeg. See
[preview support](previews.md) and [desktop compatibility](compatibility.md).

## Upgrading from Nordend

The command, desktop entry, portal backend, packages, Nix module, and environment variables
now use `rummage` / `RUMMAGE` (including helper names and `programs.rummage`).
Update launcher commands, `xdg-mime` defaults, compositor rules, portal preferences, and
Noctalia templates accordingly; the old executable and portal names are not installed as aliases.

On the first normal launch without `--config`, rummage copies existing Nordend `config.toml`,
`colors.toml`, and session `state.toml` into the corresponding `rummage` XDG directories,
only where the destination is absent. Originals and existing rummage files are left intact.
Custom commands and paths inside copied settings are preserved; update any that invoke Nordend.
Explicit `--config`, config checks, and picker/portal-only launches do not migrate files.
Older screenshots may still show the upstream name.

## Development downloads

The [latest development build](https://github.com/barrulus/rummage/releases/tag/development)
is refreshed automatically after **all checks pass on `main`**. These are unsigned Linux
x86_64 development snapshots. Choose your exact distribution; the Fedora RPM is **not
validated for Leap or RHEL**, and the DEB is not validated for Ubuntu or older Debian.

| System | Download | Install after downloading |
|---|---|---|
| Debian 13 (trixie), amd64 | [DEB](https://github.com/barrulus/rummage/releases/download/development/rummage-debian13-amd64.deb) | `sudo apt install ./rummage-debian13-amd64.deb` |
| Fedora 45, x86_64 | [RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-fedora45-x86_64.rpm) | `sudo dnf install ./rummage-fedora45-x86_64.rpm` |
| Arch Linux, x86_64 | [Arch package](https://github.com/barrulus/rummage/releases/download/development/rummage-arch-x86_64.pkg.tar.zst) | `sudo pacman -Syu`, then `sudo pacman -U ./rummage-arch-x86_64.pkg.tar.zst` |
| openSUSE Tumbleweed, x86_64 | [Tumbleweed RPM](https://github.com/barrulus/rummage/releases/download/development/rummage-tumbleweed-x86_64.rpm) | `sudo zypper dup`, then `sudo zypper install --allow-unsigned-rpm ./rummage-tumbleweed-x86_64.rpm` |
| Void glibc, x86_64 | [XBPS bundle](https://github.com/barrulus/rummage/releases/download/development/rummage-void-glibc-x86_64.tar.gz) | See the two commands below. |

For Void, extract the bundle and keep its package and repository index together:

```sh
tar -xzf rummage-void-glibc-x86_64.tar.gz
sudo xbps-install -R "$PWD/rummage-void-x86_64/packages" rummage
```

For Nix, with flakes enabled, run or install the same tested source revision through the
moving `development` tag (this builds rummage if no cached binary is available):

```sh
nix run github:barrulus/rummage/development
nix profile install github:barrulus/rummage/development
```

Enable the optional [public binary cache](#binary-cache) first to reuse CI builds.

[Checksums](https://github.com/barrulus/rummage/releases/download/development/SHA256SUMS)
and [source identity](https://github.com/barrulus/rummage/releases/download/development/source.json)
are provided with every build. To verify a download, save `SHA256SUMS` beside it and run
`sha256sum --check --ignore-missing SHA256SUMS`. Downloads are replaced as builds pass;
use the full commit from `source.json` instead of `development` in the Nix reference to
pin a version. When identical-tree manually tested branch packages are promoted after merging, `commit`
identifies the original tested checkout and `published_commit` identifies the main
commit used by the moving tag. Package bytes and source archives remain unchanged.
These downloads are individual packages, not an automatic-update repository.

For a **Debian source package**, download the
[Debian source bundle](https://github.com/barrulus/rummage/releases/download/development/rummage-debian-source.tar.gz).
It contains the matching `.dsc`, `.orig.tar.gz`, `.debian.tar.xz`, source `.changes` and
`.buildinfo`, with their versioned filenames preserved. CI builds the published DEB from
this source package. Rebuild on Debian 13:

```sh
sudo apt update
sudo apt install build-essential
tar -xzf rummage-debian-source.tar.gz
cd rummage-debian-source
dpkg-source -x rummage_*.dsc source
sudo apt build-dep ./source
cd source
dpkg-buildpackage -b -us -uc
```

These source packages are unsigned development snapshots. The
[generic source archive](https://github.com/barrulus/rummage/releases/download/development/rummage-source.tar.gz)
is also available; it is distinct from the Debian source package.

## Photo Library add-on

The standard `rummage` package includes image, text, video and PDF previews.
**Settings → General → Previews** controls video and PDF previews independently;
no separate preview packages are needed. Native upgrades replace the former
`rummage-media`, `rummage-pdf` and `rummage-previews` packages.

`rummage-photos` is the only optional add-on. Install the matching version beside
`rummage`; it owns the photo module and its SQLite dependency, and does not replace
the application's launcher. For a local Debian build:

```sh
sudo apt install ./rummage-debian13-amd64.deb ./rummage-photos-debian13-amd64.deb
```

For Fedora, install both matching RPMs in one command from the directory containing
the downloaded packages (these filenames are for the Fedora 45 build):

```sh
sudo dnf install ./rummage-fedora45-x86_64.rpm ./rummage-photos-fedora45-x86_64.rpm
```

There are two packages: the application and its optional photo add-on. The add-on
requires the exact matching application version. If that version of `rummage` is
already installed, only the add-on RPM needs to be supplied. These filenames
describe the packaging output; local branch builds are not published downloads.

For Arch and Tumbleweed, pass the matching two packages to `pacman -U` or
`zypper install --allow-unsigned-rpm`. For Void, install `rummage-photos` from the
same indexed bundle.

After installing:

1. Close and reopen Rummage.
2. In **Settings → General**, check that **Enable Photo Library** is on (the default).
3. Choose **Photo Library** in the sidebar.
4. Open the toolbar's **Photo Library** menu and choose **Add folder…**. Select a
   source in Rummage's folder picker, review the subfolder and exclusion options,
   and confirm to start indexing.

Installing the add-on does not scan any folder automatically. Originals stay in
their existing locations. See the [Photo Library guide](photo-library.md) for
albums, saved searches, annotations, duplicates and background activity.

Disable **Enable Photo Library** in General settings to stop using the capability.
Removal or disabling preserves catalog data and annotations, and ordinary file
browsing remains available.

For Nix, `nix build .#rummage-with-photos` selects the application with the module;
`.#rummage-photos` selects the module output for custom composition. The wrapper
sets `RUMMAGE_PHOTOS_LIBRARY` to its exact module path. A custom installation can
set that variable to an absolute `librummage-photos.so` path. Source builds use
`-DRUMMAGE_PHOTOS=ON` and SQLite development headers. Photo support defaults to OFF;
`cmake --install build --component photos` installs the module only.

## Optional video previews

Video thumbnails and still previews are included in the standard package and
controlled by `previews.media` (default `true`) or **Video previews** in Settings.
Quick Look supports [audio/video playback](previews.md#audio-and-video-playback); Enter opens the default player.
The grid draws filmstrip borders; cached images and larger previews stay undecorated.

The isolated helper uses system FFmpeg 6.1 or newer libraries (`libavformat`,
`libavcodec`, `libavutil`, `libswscale`, `libswresample`); it does not execute FFmpeg command-line
programs. Codec availability follows the distribution build. Turning the setting
off prevents new helper requests; in-flight work finishes and already cached images
may remain. The application itself does not link FFmpeg. A separate audio helper links `libpulse-simple` for PCM output.

Source builds default to `RUMMAGE_MEDIA=ON`. Developers can set it to OFF for a
lean build; this is not a separate distributed package. Separate custom helper
prefixes can still be registered through `RUMMAGE_ENGINE_PATH`, a colon-separated
list of absolute `share/rummage/engines` directories. Restart after changing the
installed engines.

## Optional PDF previews

PDF thumbnails and navigable Quick Look pages are included in the standard package and
controlled by `previews.pdf` (default `true`) or **PDF previews** in Settings.
Password-protected PDFs get a lock on their icon, damaged ones a cross; see
[previews](previews.md#optional-pdf-previews). Enter opens the default document viewer.

The isolated helper uses PDFium without JavaScript or XFA support. Native packages
include a private `libpdfium.so` beside the helper and its license notices; they
do not replace a system PDFium library. The application itself does not link PDFium.
Turning the setting off prevents new helper requests; in-flight work finishes and
already cached images may remain.

Source builds default to `RUMMAGE_PDF=ON`. Point CMake at PDFium using `PDFium_DIR`
or `CMAKE_PREFIX_PATH`, or install its headers and library on the usual search
paths. Developers can set the switch to OFF for lean builds. For source-package
builds and renderer maintenance, see the
[package builder guide](../contributing/ci-packaging.md#native-pdf-previews).

## Nix

With flakes enabled, from this checkout:

```sh
nix build
nix run . -- ~/Pictures
nix flake check
```

`nix build` produces `result/bin/rummage` plus the preview helper, manifests and
desktop integration files. The ordinary package omits tests.
CI runs the full test suite before starting any package job. The test derivation
(including media and PDF) produces only a success marker, which the Nix package job
reuses without rerunning the suite. `nix flake check` also runs installed
package smoke checks and checks the NixOS module. Packages and development shells are
defined for `x86_64-linux` and `aarch64-linux`; checking one host is not evidence of
runtime testing on both architectures.

On non-NixOS hosts, running a Nix-built GUI also requires access to the host's
graphics drivers. A successful package build alone does not establish that setup.

### Binary cache

The public [rummage-fm cache](https://rummage-fm.cachix.org) provides **x86_64-linux**
builds from main and manually tested repository branches. It includes the default package,
media-only, PDF-only and combined preview variants, including their helper outputs
and runtime dependencies. The cache starts filling after the first successful CI
upload; older revisions and aarch64 builds may require local compilation. Completed
dependencies are uploaded even if a later application check fails. Use `development`
to select a revision whose complete release checks passed.

The flake advertises this cache and its public signing key through `nixConfig`.
When running commands such as `nix run`, Nix prompts to accept these settings
unless you have already configured whether to trust them. They extend your existing
cache settings. Accept them to use cached builds when available.

To configure the cache independently of the flake, enable it once with Cachix installed:

```sh
cachix use rummage-fm
```

Or run the client without installing it permanently:

```sh
nix shell nixpkgs#cachix -c cachix use rummage-fm
```

For NixOS, add these settings to your system configuration and rebuild instead:

```nix
nix.settings = {
  substituters = [ "https://rummage-fm.cachix.org" ];
  trusted-public-keys = [
    "rummage-fm.cachix.org-1:fhPhaCttVfPBS/i//+Z+6Ch0xdxj9g94eZdQ5m6mlwA="
  ];
};
```

Keep any existing cache entries. On a shared multi-user Nix installation, the
administrator may need to configure the daemon's cache trust; follow the client's
instructions. Reading this public cache requires no Cachix account or token.

Continue using the same `nix run`, `nix build`, profile or NixOS configuration
commands. Nix downloads matching signed outputs when available and builds missing
ones normally. Changing the source, package options or pinned dependencies can
produce a different store path and require a build.

### NixOS configuration

Add an input to your existing system flake. The fork remote configured for this
checkout is `barrulus/rummage`:

```nix
inputs.rummage.url = "github:barrulus/rummage";
```

For unpublished local work, use `path:/absolute/path/to/rummage-fm` instead. A remote
input only contains work pushed to that remote; it cannot include local edits.

Include `rummage` in your existing output arguments and import its module:

```nix
outputs = { nixpkgs, rummage, ... }: {
  nixosConfigurations.my-host = nixpkgs.lib.nixosSystem {
    system = "x86_64-linux";
    modules = [
      ./configuration.nix
      rummage.nixosModules.default
      {
        programs.rummage = {
          enable = true;
          photos.enable = true;
          filePicker.enable = true;
        };
      }
    ];
  };
};
```

Keep your existing inputs, outputs and modules; replace `my-host` with the actual
host name. Apply from your configuration directory:

```sh
sudo nixos-rebuild switch --flake .#my-host
```

Video and PDF previews are included automatically and can be toggled in Settings.
`photos.enable` defaults to `false` and selects the Photo Library add-on.
`programs.rummage.package` can select another package. Enabling photos overrides
that package's `withPhotos` argument, which custom packages must support.
Enabling the application
alone does not change portal preferences. For Open/Save dialogs also enable
`programs.rummage.filePicker.enable`; see the [picker guide](file-picker.md) for
desktop selection and preserving other portal backends.

### Home Manager

The flake exports a NixOS module, not a Home Manager module. To install the package
through Home Manager, pass `inputs` using `extraSpecialArgs` in your existing
`homeManagerConfiguration`, or `home-manager.extraSpecialArgs` in a NixOS setup:

```nix
extraSpecialArgs = { inherit inputs; };
```

Then add to your home configuration:

```nix
{ inputs, pkgs, ... }: {
  home.packages = [ inputs.rummage.packages.${pkgs.stdenv.hostPlatform.system}.default ];
}
```

Apply with `home-manager switch --flake .#your-user`, or rebuild NixOS when it
manages Home Manager. Installing the package does not select it as a portal backend.

## Build dependencies

Source statistics use `nlohmann_json >= 3.11` (required to build) and Tokei (optional at runtime
on native packages, included by Nix). Install `tokei` and make it available on
`PATH` to enable the Languages view.

[CMakeLists.txt](../CMakeLists.txt) is the authoritative list for this checkout.
Use a C++20 compiler and standard library with `std::format`, CMake 3.20 or newer,
Ninja (for the examples), and pkg-config. Required library/module names are:

| Area | CMake/pkg-config requirements |
| --- | --- |
| Display | `wayland-client`, `wayland-egl`, `wayland-cursor`, `egl`, `glesv2`, `xkbcommon` |
| Protocol generation | `wayland-scanner`, `wayland-protocols` including xdg-dialog-v1 and stable tablet-v2 XML |
| Text | `harfbuzz`, `freetype2`, `fontconfig` |
| Configuration | `tomlplusplus` |
| Source statistics JSON | `nlohmann_json >= 3.11` |
| Filename regular expressions | `re2` |
| Images | `libwebp`, `libwebpdemux`, `libjxl`, `libheif >= 1.23`, `lcms2`, `zlib` |
| SVG theme icons and file previews | `librsvg-2.0 >= 2.52`, `pangocairo`, `pangoft2`, Cairo |
| Desktop services | `sdbus-c++` |
| Media helper | FFmpeg 6.1+ development libraries: `libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample` and `libpulse-simple`; `RUMMAGE_MEDIA` defaults to ON |
| Embedded terminal (enabled by default) | Paul Evans libvterm, pkg-config `vterm >= 0.3`; disable with `RUMMAGE_TERMINAL=OFF` |
| Tests | `doctest`; `dbus-run-session` for portal/notification integration tests |

librsvg is implemented partly in Rust, but rummage links its packaged C API;
building rummage against installed libraries requires no Rust compiler or Cargo.
Building librsvg itself from source does require its own Rust toolchain.

HEIC decoding requires libheif's **libde265** decoder at runtime (built in or
installed as a codec plugin) and Little CMS 2. The Nix development shell/package
supplies them. The preview helper initializes codecs and decodes a trusted HEIC
fixture before entering its sandbox; later decoding adds no sandbox permissions.

Optional reference dependencies are libjpeg-turbo for the JPEG benchmark, LibRaw
for DNG comparison tests, and exiv2 for metadata comparison tests/benchmarks.
These reference libraries are not required to build the application.

Distribution package names vary. The [packaging recipes](../contributing/ci-packaging.md) contain
platform-specific dependencies. Use the Nix development shell when maintaining
local build dependencies is inconvenient.

## Build from source

Run from the checkout you intend to build:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
ctest --test-dir build --output-on-failure
./build/rummage ~/Pictures
cmake --install build --strip
```

Install doctest before configuration if you want tests: CMake skips them with a
notice when it is missing. See [development](../contributing/development.md) for test environment
requirements and build options. Keep the helper and engine manifests alongside the
application in the build tree or installation; copying only `rummage` loses previews.

The install prefix contains:

| Path | Purpose |
| --- | --- |
| `bin/rummage` | File manager and portal backend |
| `libexec/rummage/rummage-preview` | Isolated image/text reader |
| `share/rummage/engines/base.toml` | Engine registration |
| `share/applications/rummage.desktop` | Launcher and folder association |
| `share/icons/hicolor/scalable/apps/rummage.svg` | Application icon |
| `share/rummage/noctalia/rummage` | Noctalia colour template |
| `share/rummage/matugen/rummage.toml` | Matugen colour template |
| `share/rummage/config.example.toml` | Complete annotated settings reference |
| `share/xdg-desktop-portal/portals/rummage.portal` | Portal registration |
| `share/dbus-1/services/org.freedesktop.impl.portal.desktop.rummage.service` | On-demand portal activation |

The annotated configuration is installed at `share/rummage/config.example.toml`.
A first normal launch creates a personal config only if absent. Existing personal
files are never replaced by updates. Ensure the prefix's `bin` is on `PATH` and
its desktop data is discoverable. The portal service uses an absolute executable
path generated for the install prefix.

## Build distribution packages

Use the [package builder guide](../contributing/ci-packaging.md) for Void glibc, Debian 13, Fedora
44, Arch Linux and openSUSE Tumbleweed. Those builders archive a clean committed
checkout, record its identity and validate installation in disposable containers.
They reject uncommitted changes; their output directory must be outside the checkout.
The Void template is populated by the builder with an archive and checksum rather
than downloading an upstream release. Ordinary CMake builds need no container.

## Desktop setup and updates

To make rummage the default application for folders:

```sh
xdg-mime default rummage.desktop inode/directory
```

Assign `rummage` to a compositor shortcut if desired. For Umbriel's `config.toml`:

```toml
"Mod+E" = { action = "spawn rummage", repeat = false }
```

[Noctalia colours](appearance.md#noctalia-colours) and
[file-picker integration](file-picker.md) have separate setup steps. Setting the
folder association does not change application file dialogs.

To update a source install, rebuild and reinstall from the intended revision using
the same prefix. For a flake input, update its lock entry in your configuration and
rebuild. Personal `config.toml` files are never rewritten to add new options; compare
with the current reference. Record `rummage --about` when diagnosing differences
between installed and development builds.

## Shell completions

Packages install completions for bash, zsh and fish. Bash uses bash-completion;
zsh uses `compinit`; fish loads its vendor completions automatically. The options
follow the selected mode: picker options appear after `--pick`, `--pick-directory`
or `--save-file`. Folder arguments complete directories; `--reveal` and
`--properties` also complete files. Source builds can omit these files with
`-DRUMMAGE_INSTALL_COMPLETIONS=OFF`.

The optional model helper uses system Assimp (`assimp` via pkg-config); the UI does
not link it. `-DRUMMAGE_MODEL=ON` builds this helper, and native packages split it
into `rummage-model`. The Nix outputs `rummage-model` and `rummage-with-model` provide
the helper alone or the application with it; `rummage-with-previews` includes it.
See [model format coverage](previews.md) before choosing this optional component.

NixOS module users can independently enable `programs.rummage.preview.font` and
`programs.rummage.preview.model`; both default to false. Media/PDF support is included in the base package.
