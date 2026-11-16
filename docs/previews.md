# Previews and formats

[Documentation](README.md) · [Security model](security.md) · [Troubleshooting](troubleshooting.md)

The grid and information pane show thumbnails/previews. **Space** opens Quick Look
for the current file; arrows move through the folder, and Space/Escape closes it.
Unsupported files get an information card or a reason why a preview is unavailable.
Opening a file with Enter uses its desktop application, independent of preview support.

**Alt+Enter** opens Properties with a compact metadata card and an image or video
thumbnail beside the filename when available. Unsupported files use their file-type
icon. Its width follows the filename and metadata instead of reserving an empty
right-hand column; long values are ellipsized within the window. The dialog grows
for folder-icon controls, link targets and errors; in short
windows, its contents scroll while the action buttons remain visible.

## Format support in this source tree

| Family | Supported path | Limits |
| --- | --- | --- |
| PNG, JPEG, GIF | wuffs decoding in the preview helper | A picture preview, not an animation player |
| WebP | libwebp in the helper | Still preview |
| JPEG XL | libjxl in the helper | Still preview |
| HEIC / HEIF | libheif with libde265 for HEVC; Little CMS for RGB ICC conversion | Primary still only; HEVC containers, not arbitrary HEIF codecs |
| TIFF / BigTIFF | rummage's TIFF reader | Compression/sample limits below |
| PGM / PPM | rummage's binary PNM reader | 8- and 16-bit samples; not every Netpbm format |
| Camera raw | Embedded previews; some DNGs developed by rummage | Support depends on the file's container, preview and compression |
| Text | Isolated decoding, syntax colours and selectable text | Bounded reads; not an editor or rendered Markdown viewer |
| Video | Optional `rummage-media` engine using FFmpeg libraries | Thumbnails and still previews; no playback |
| PDF | Optional `rummage-pdf` engine using PDFium | First page as thumbnail and still; no document viewer |
| SVG theme icons | rummage's in-process icon renderer | Supported subset described below; SVG files use text previews |
| Other formats | File information and external application opening | No built-in audio player, archive-content or font preview |

The [base engine manifest](../data/engines/base.toml) registers MIME families.
Registration is not a guarantee that every subtype/sample can decode. The image
loader checks bytes rather than trusting an extension. A detected type must also
reach an appropriate provider; system MIME data matters.

### SVG

SVG files open as text in Quick Look. Rendered SVG document previews and
thumbnails are not currently available.

SVG theme icons use rummage's in-process renderer. It supports paths and shapes,
strokes, gradients, basic CSS, groups, clipping, masks, colour matrices, Gaussian
blur, patterns and embedded PNG/JPEG. It preserves transparency and aspect ratio,
with input capped at 8 MiB and rendered icons at 1024 pixels per side.

This is a static SVG subset: text, animation, scripts, external images/styles and
other external resources are not rendered or fetched. Unsupported filter
primitives leave the element unfiltered. Malformed documents and exhausted
rendering limits return no rendered icon, allowing the icon fallback to be used.
See the [security model](security.md) for parsing boundaries.

### TIFF

TIFF supports uncompressed, LZW, Deflate, PackBits and JPEG data, strips and tiles,
BigTIFF, integer and floating-point samples, and common grey, RGB, YCbCr, palette,
CMYK and alpha layouts. Unsupported or excessive inputs report a preview error.

### HEIC and HEIF phone photos

The primary still appears in Quick Look, the information pane and thumbnails,
including tiled images, container rotation/mirroring, clean aperture crops and
transparency. Container transforms apply once; EXIF orientation is not reapplied.
RGB ICC profiles (including Display P3) and common NCLX profiles convert to 8-bit
sRGB with straight alpha. Untagged/unspecified colour is assumed sRGB.

PQ uses 203-nit SDR white; HLG uses a fixed 1000-nit display and system gamma 1.2,
mapped to the same SDR white. Highlights above SDR white and colours outside
sRGB clip. There is no adaptive tone mapping, gain-map reconstruction, full HDR
output or display-profile management. Sequences, depth and other auxiliary images
are not displayed. HEIF codecs other than HEVC are unsupported by this reader.
Invalid inputs, unsupported profiles/codecs and resource limits produce a preview
error. See [build dependencies](install.md#build-dependencies) for codec requirements.

### Camera raw and DNG

rummage prefers the camera's stored JPEG: this preserves the camera's rendering
and avoids developing full raw data for every thumbnail. Its readers handle
previews in ORF, CR2, NEF, ARW, RAF, RW2, SRW, ERF, 3FR and DNG families, among
others. Previews are oriented and cropped where metadata allows; broken candidates
can be skipped in favor of another stored preview.

When a DNG's previews are too small for the requested display, rummage can develop
supported uncompressed or lossless-JPEG sensor data itself. That path handles
black/white levels, white balance, colour matrices, crop, exposure/tone information
and supported gain-map/lens-warp corrections. It is a viewing pipeline, not a raw
editor. Unhandled corrections or encodings can require a stored preview instead.

CRW and MRW are examples of registered formats without a supported reader; Sigma
X3F and older Panasonic files may have no readable preview. A file extension alone
cannot establish camera support. Include the camera/model and a reproducible sample
when reporting a preview failure.

Photo metadata includes camera, lens, exposure, aperture, ISO, focal length and
capture time when available. The current reader does not read star ratings from
darktable sidecars. Sidecar file operations are described in
[file operations](file-operations.md#darktable-sidecars).

## Text Quick Look

Text opens on a scrollable page in the configured monospace font. Supported
decoding includes UTF-8, UTF-16 with a byte-order mark, and a Windows-1252/Latin-1
fallback. The caption identifies language, encoding, line endings and size/lines.

Syntax colours cover C/C++, Rust, Python, JavaScript/TypeScript, Lua/Luau, shell,
fish, CMake, Makefiles, JSON, TOML, YAML, INI/desktop/systemd files, ignore and
attributes files, Markdown and diffs. Detection uses MIME, filename/extension and
shebang as appropriate. Markdown is source with syntax colours; fenced code can
use its own language. Minified files with long average lines are shown plain.

Drag to select, double-click a word, triple-click a line, or Shift+click to extend.
**Ctrl+A** selects the loaded text and **Ctrl+C** copies the text selection. With
no text selected, Ctrl+C keeps the file-copy behavior. Selection can auto-scroll.

Home/End goes to the top/bottom and Page Up/Down scrolls a page. Shift+wheel or
horizontal touchpad scrolling handles long lines while line numbers stay fixed.
Arrow keys still move between files. Clicking outside the page closes Quick Look.

Reads are bounded. Large `.log` files show the last 256 KiB and open at the bottom;
the caption identifies the tail. Do not assume a selected preview contains every
byte of a large source file.

## Text in the information pane

The information pane shows a text file as a page of up to 30 lines, shrinking to fit
shorter windows. It is coloured as in Quick Look and scrolls through everything
Quick Look would read (the same bounded read, and a large `.log` opens at its newest
lines). The wheel or touchpad over the page scrolls the page; anywhere else on the
pane it scrolls the pane. **Alt+J** and
**Alt+K** scroll three lines down and up, **Alt+Page Down** and **Alt+Page Up** a page.
Drag to select text, double-click a word, triple-click a line, or Shift+click to
extend. Clicking the page gives it text focus: **Ctrl+A** selects the loaded text
and **Ctrl+C** copies the selection. Home/End and Page Up/Down scroll the focused
page. Shift+wheel or horizontal touchpad scrolling reveals long lines. Dragging
beyond the page scrolls the selection.

The filename and metadata are also selectable, including values under **More
details**. Long names and values wrap in full; copying keeps the original text
without the visual line breaks. Click a field, then use **Ctrl+A** to select that
field and **Ctrl+C** to copy it. Clicking the file view returns these shortcuts to
file selection and file copying. Changing the selected file or hiding the pane
clears text focus and selection. All preview and metadata text remains read-only.

## Optional video previews and caching

Install the matching `rummage-media` package for video thumbnails and still previews
in the information pane and Quick Look. The grid decorates video thumbnails with
film rails; cache images and larger stills stay undecorated. This is not playback.
Enter opens the file in its default player.

The helper links system FFmpeg 6.1+ libraries and uses the same descriptor-based
seccomp/Landlock isolation as other engines. It does not execute FFmpeg command-line
programs. Decoding has bounded time, dimensions and concurrency and tries beyond
dark opening frames. Codec availability follows the installed distribution libraries.

The core works without the helper and can still reuse valid cached thumbnails.
Missing helper/libraries do not create permanent failure entries. Restart after
installing or changing engines. See [optional media installation](install.md#optional-video-previews)
for CMake, Nix and `RUMMAGE_ENGINE_PATH` registration.

## Optional PDF previews

Install the matching `rummage-pdf` package for PDF thumbnails and first-page stills in
the information pane and Quick Look. Enter still opens the document in its default
application; this is a preview of page one, not a viewer.

The helper links PDFium and uses PDFium's built-in fonts with an empty system font
list, so it opens no files inside the same seccomp/Landlock isolation as the other
engines. PDFium parses only PDF: other formats named `.pdf` are refused. The build
has no JavaScript engine (no V8, no XFA), and stream data in external files is never
read. Memory is bounded by the helper's 4 GiB address-space limit, reading is bounded
in bytes and time; exhausted reads discard the preview, even when PDFium recovers from
the read error. Page drawing is progressive and stops at a deadline. Saved AcroForm
text fields and checkbox appearances are drawn afterwards without executing form actions;
this step is bounded by the parent helper timeout. Pages are drawn at
most 2048 pixels per side, and requests get the media engine's 8 second timeout and
smaller pool. JPEG 2000 scans decode only the resolution the thumbnail needs.

The built-in fonts do not cover all scripts: PDFs with non-embedded CJK fonts may have
missing text. Embedded fonts are supported. XFA forms are not supported.

A password-protected PDF keeps its icon with a lock badge and Quick Look says so; a
PDF with an empty user password (owner restrictions only) previews normally. A
damaged PDF keeps its icon with a cross badge. The reason is remembered with the
cached failure until the file changes. See
[optional PDF installation](install.md#optional-pdf-previews).

Thumbnails use the shared freedesktop cache under `$XDG_CACHE_HOME/thumbnails`
(normally `~/.cache/thumbnails`). Entries are associated with file URI and
modification metadata; failed previews can be cached too. This cache is shared
with other applications, not a private photo library.

## Engines and failure behavior

Image and text requests go through `rummage-preview`. A crashed, timed-out or
unavailable helper should lose the preview rather than the browser. Engines are
registered by manifests found beside the build executable or under
`share/rummage/engines`; `rummage --about` lists discovered engines. A missing helper
or invalid manifest prevents that engine's previews.

See [architecture](../contributing/architecture.md#preview-engines) for developer details and
[troubleshooting](troubleshooting.md#previews-are-unavailable) for installation and
runtime diagnosis. Format-specific limits and process isolation reduce exposure;
they do not imply all file handling runs in a sandbox.

## Photo and video details

In Quick Look, click **Details** or press **I** to open a scrollable information panel.
Values wrap and can be selected and copied with Ctrl+C. The panel follows the selected file
and also works in narrow windows. It includes file type, size, dates, permissions and location.

Photo EXIF includes camera, lens, shutter speed, aperture, ISO, focal length, capture time,
exposure program/compensation, metering, white balance, flash and software when present.

With the optional media helper installed, video previews also expose container, duration,
upright resolution, video codec, frame rate, bitrate, pixel format, rotation, audio codec,
channel count, sample rate and embedded title/artist/recording-time/encoder tags. These values
also appear in the information pane. Missing fields are omitted; missing media support leaves
basic file information available. Video parsing stays inside the sandboxed helper.
