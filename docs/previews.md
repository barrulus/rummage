# Previews and formats

[Documentation](README.md) · [Security model](security.md) · [Troubleshooting](troubleshooting.md)

Grid, Details and Columns show thumbnails in place of file icons wherever supported.
**View → Show → Thumbnails** switches all file views between thumbnails (on by default)
and file-type icons, and remembers the choice between sessions. Previews fit inside
the existing icon space and grow with the view's zoom. Unsupported files and previews
still loading keep their normal icons. The information pane and Quick Look remain
available when file previews are off.

**Space** opens Quick Look
for the current file; arrows move through the folder, and Space/Escape closes it.
Unsupported files get an information card or a reason why a preview is unavailable.
Opening a file with Enter uses its desktop application, independent of preview support.

Quick Look uses the original image or video dimensions, when known, to size both the
thumbnail and the sharp preview. Small images stay at their native size; reduced video
frames and embedded raw previews can fill more of the window, up to the original size.
Older cached thumbnails without dimensions still fill the preview area until the sharp
image arrives.

The information pane starts with file information, dimensions and a short media
summary (container, duration and video codec when available). **More details**
reveals the remaining media metadata and filesystem details. **Copy all properties**
stays at the top of the pane and includes collapsed fields in the copied text.

**Alt+Enter** opens Properties with a scrollable metadata card and an image or video
thumbnail beside the filename when available. Unsupported files use their file-type
icon. Its width follows the filename and metadata instead of reserving an empty
right-hand column; long labels and values wrap within the window and can be selected
and copied. File information, dimensions, container, duration and video codec appear
immediately; expand **Media details** for the remaining media metadata. The copy icon
at the top copies all properties, including collapsed details, as labeled plain text.
The dialog grows for folder-icon controls, link targets and errors; in short windows,
its contents scroll while the header and action buttons remain visible.
Use the wider scrollbar to drag through long metadata, or click its track to jump.
Wheel scrolling moves several lines per notch; touchpad scrolling follows the
reported pixel movement.

## Format support in this source tree

| Quick Look over the dark browser | Quick Look over the light browser |
| --- | --- |
| ![The same illustration in Quick Look over the dimmed dark browser](images/rummage-dark-quicklook.webp) | ![Colourful character illustration in Quick Look over the dimmed light browser](images/rummage-light-quicklook.webp) |

| Family | Supported path | Limits |
| --- | --- | --- |
| PNG, JPEG, GIF | wuffs decoding in the preview helper | A picture preview, not an animation player |
| WebP | libwebp in the helper | Still preview |
| JPEG XL | libjxl in the helper | Still preview |
| HEIC / HEIF | libheif with libde265 for HEVC; Little CMS for RGB ICC conversion | Primary still only; HEVC containers, not arbitrary HEIF codecs |
| TIFF / BigTIFF | rummage's TIFF reader | Compression/sample limits below |
| PGM / PPM | rummage's binary PNM reader | 8- and 16-bit samples; not every Netpbm format |
| Camera raw | Embedded previews; some DNGs developed by rummage | Support depends on the file's container, preview and compression |
| Text | Isolated decoding, syntax colours, selectable text and rendered Markdown in Quick Look | Bounded reads; not an editor |
| Video | Included `rummage-media` helper using FFmpeg libraries | Thumbnails, still previews and Quick Look playback |
| PDF | Included `rummage-pdf` helper using PDFium | First-page thumbnails; page navigation and zoom in Quick Look |
| SVG files | librsvg in the preview helper | Static previews and thumbnails; limits described below |
| ZIP, TAR, tar.gz, gzip | Archive summary and member previews through the base helper | Read-only browsing; encrypted or unsupported members require external extraction |
| Fonts | Optional `rummage-font` engine using FreeType | Scalable TrueType/OpenType specimens; first face of a collection |
| Other formats | File information and external application opening | No built-in audio player; unsupported model/art formats listed below |

The [base engine manifest](../data/engines/base.toml) registers MIME families.
Registration is not a guarantee that every subtype/sample can decode. The image
loader checks bytes rather than trusting an extension. A detected type must also
reach an appropriate provider; system MIME data matters.

For archive browsing, copying members out and creating archives, see
[Browse and create archives](actions.md#browse-and-create-archives).

### SVG

SVG files show as pictures in Quick Look, the information pane and thumbnails,
using librsvg through its C API inside `rummage-preview`'s sandbox. Theme icons
use the same rendering adapter in the application process.

The renderer supports paths and shapes, strokes, gradients, CSS selectors, groups,
clipping, masks, SVG filters, patterns, embedded images and text.
It preserves transparency and aspect ratio. Input is capped at 8 MiB and the
rendered preview at 1024 pixels per side; larger display requests use that bounded
preview. The preview's pixel dimensions are not reported as intrinsic document
dimensions.

Before sandboxing, the helper prepares the system's generic sans-serif, serif and
monospace faces in regular/bold and upright/italic styles. Text uses that bounded
set; other families fall back, and scripts absent from those fonts may have missing
glyphs. Font data stays in memory, with no font-file access during rendering.

Animation and scripts are not executed. External images, stylesheets and other
resources are not fetched: documents have no base URI. Data URIs and internal
references remain available. Compressed SVG (`.svgz`) is not supported. Compatibility
depends on the installed librsvg version; full CSS custom-property resolution is
not guaranteed. Malformed documents and exhausted helper limits produce a preview
error. Complex SVGs can exceed the base helper's three-second request deadline.
See the [security model](security.md) for preview isolation and its boundaries.

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
shebang as appropriate. Markdown opens rendered in Quick Look; **M** or the **Source / Rendered** button switches
between the page and highlighted source. Fenced code can use its own language. Minified files with long average lines are shown plain.

Drag to select, double-click a word, triple-click a line, or Shift+click to extend.
**Ctrl+A** selects the loaded text and **Ctrl+C** copies the text selection. With
no text selected, Ctrl+C keeps the file-copy behavior. Selection can auto-scroll.

Home/End goes to the top/bottom and Page Up/Down scrolls a page. Shift+wheel or
horizontal touchpad scrolling handles long lines while line numbers stay fixed.
Arrow keys still move between files. Clicking outside the page closes Quick Look.

Reads are bounded. Large `.log` files show the last 256 KiB and open at the bottom;
the caption identifies the tail. Do not assume a selected preview contains every
byte of a large source file.

## Rendered Markdown

Quick Look renders headings, bold and italic text, inline code, lists and task boxes,
quotes, simple pipe tables, links and fenced code blocks. Paragraphs and table cells
wrap to the window; code uses the monospace font with syntax colours. **M** switches
between rendered text and the original source. The button at the top offers the same
switch. Home/End and Page Up/Down scroll either view; arrow keys still move between files.
Select rendered text with the pointer or Ctrl+A, then copy with Ctrl+C. Copying rendered
text omits markup; switch to Source to copy the original Markdown.

This is a bounded Markdown subset, not a web page: HTML stays literal, links are styled
text without navigation, and images show their alternative text as placeholders. Nothing
fetches external resources. Reference links, deeply nested formatting and advanced Markdown
extensions may remain literal. Parsing runs in the text preview helper; truncated files or
presentation limits show **Preview truncated**. The information pane keeps highlighted source.

The information pane follows the focused file during multi-selection. Selection
totals appear beneath its metadata; each browsing pane keeps its selection count
in its upper-right header. Automatic visibility is available under **View → Show**;
it hides only when no item is focused, not when an image decoder is unavailable.

## Text in the information pane

The information pane shows a thumbnail of the first 12 lines, shrinking to fit
shorter windows. It reads at most 16 KiB and clips long lines. Logs also start at
the beginning here. Press **Space** to open Quick Look for reading, scrolling and
copying text. The thumbnail itself does not scroll or select text.

The wheel or touchpad scrolls the information pane as a whole. **Alt+J** and
**Alt+K** scroll down and up; **Alt+Page Down** and **Alt+Page Up** move a page.

The filename and metadata are also selectable, including values under **More
details**. Long names and values wrap in full; copying keeps the original text
without the visual line breaks. Click a field, then use **Ctrl+A** to select that
field and **Ctrl+C** to copy it. Clicking the file view returns these shortcuts to
file selection and file copying. Changing the focused file or hiding the pane
clears text focus and selection. All preview and metadata text remains read-only.

## Optional video previews and caching

![Video thumbnails with filmstrip borders and a selected recording's still preview and metadata](images/rummage-dark-video-grid.webp)

Video thumbnails and still previews in the information pane and Quick Look are
included in the standard package. Toggle **Video previews** in General settings. The grid decorates video thumbnails with
film rails; cache images and larger stills stay undecorated. Quick Look also offers [audio/video playback](#audio-and-video-playback).
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

PDF thumbnails and Quick Look are included in the standard package. Toggle **PDF previews** in General settings. The
information pane and thumbnail cache continue to use page one. In Quick Look:

- Click **‹ / ›** or press **Page Up / Page Down** to change page; **Home / End**
  goes to the first / last page. The toolbar shows the current page and total.
- Click **− / +**, press **− / +** (or **=**), or use **Ctrl+wheel** to zoom from
  fit to 400%. **0** resets to fit. Wheel scrolls vertically; **Shift+wheel** pans
  horizontally when the zoomed page extends outside the preview.
- Arrows still change files; Space/Escape closes Quick Look. Enter opens the default
  application.

Pages load on demand. Changing page cancels the superseded request and retains only
one page per cached file; the existing bounded neighbour-file cache is shared with
other previews. The raster is limited to 2048 pixels per side, including at high zoom.
This preview does not edit documents, select PDF text, follow links, search, fill
forms, or unlock password-protected documents.

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

If a thumbnail helper stops or times out, visible thumbnails retry twice after
short delays. Leaving the view cancels pending retries; returning tries again.
These temporary failures are not saved to disk. With `RUMMAGE_LOG=debug`, failed
thumbnail attempts log the file, requested size and underlying error.

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


### Extended file metadata

Properties, the information pane and Quick Look Details expose the same available media
fields. Image details include aspect ratio and source format information such as bit depth,
color model, compression and interlacing when the reader can determine them. These describe
the source, rather than the preview's converted RGBA pixels.

PNG text fields (including compressed and international text), color/profile information,
pixel density, timestamps and animation headers are included. JPEG comments and XMP, and
TIFF/EXIF directories including GPS and otherwise unrecognized tags, are also exposed.
Unknown TIFF tags retain their numeric identifiers; binary blocks and oversized arrays show
summaries rather than decoded contents. WebP, GIF, JPEG XL and HEIF expose technical header
information; HEIF also exposes its embedded metadata blocks. Availability varies by format:
this is not a complete interpretation of proprietary maker notes, IPTC, ICC profiles or XMP.

Video details include container tags, each stream's tags and codec information, color fields,
audio channel layouts and chapter metadata. With the PDF helper installed, document details
include version, page count, standard document information and the first page's size in points.
Text details include language, encoding, line endings and loaded line count; archive details
include contents, unpacked size and reading/encryption limitations. Missing values are omitted.

Metadata reads remain bounded inside the preview helpers. Extended details are limited to
256 fields, 4096 bytes per label/value, and a 64 KiB helper response; format readers also limit
bytes and directory/chunk traversal. Truncated metadata is identified, and binary summaries
explicitly mark omitted data. Text preview line counts are labeled as preview counts when the
file was not read in full. No sidecar files are read for these details.


## Media columns in Details

The Details header menu offers **Dimensions**, **Duration**, **Bitrate**, **Video
codec**, and **Audio codec** alongside filesystem columns. Choices, order, widths
and sort key are remembered globally, per folder and per tab. Dimensions sorts by
pixel area; duration and bitrate sort numerically; codec names use natural ordering.
Unavailable values stay after known values in both ascending and descending order.

An ellipsis means metadata is pending; a dash means the field is absent, unsupported,
or could not be read (including a missing optional helper). Images use the base
helper; audio and video require `rummage-media`. The media helper also reads common
MP3, FLAC, Ogg, WAV, AAC, Opus and MPEG-4 audio metadata; support depends on the
container and codecs available in the linked FFmpeg build.

Visible rows request metadata asynchronously. Sorting by a media field gradually
loads the folder so rows can move as values arrive; selection follows filenames.
Each folder queues at most 256 requests and processes one batch of eight at a time,
using the existing bounded helper pool and timeouts. Completed and unavailable
results are cached for the entry's lifetime and invalidated by file changes. Closing
the folder cancels pending work; an already active metadata request ends at the
helper's timeout. There is no persistent media index or recursive background scan.
Archive members currently show dashes in these columns.

## Optional font specimens and additional format coverage

Source builds can enable `-DRUMMAGE_FONT=ON` to build `rummage-font`, which depends on
FreeType. Build target `rummage-font`, then install CMake component `font` alongside
the matching application. The helper and `font.toml` manifest use the normal
`libexec/rummage` and `share/rummage/engines` locations. Restart after installation.
The native package recipes also provide a separate `rummage-font` package; the
`rummage-previews` convenience package includes it. With Nix, use
`nix build .#rummage-with-font` for the application with the registered helper, or
`.#rummage-font` for the helper output alone. `.#rummage-with-previews` includes media, PDF and font helpers.

Font parsing and rasterization run inside the same isolated preview helper cage.
Input is limited to 32 MiB, output to 1024 pixels per side; the ordinary helper pool,
timeout, cancellation, thumbnail cache and scale handling apply. Quick Look shows a
fixed Latin/digit specimen for scalable TrueType/OpenType fonts and the first face
of a font collection. Bitmap-only fonts and additional collection faces are not
shown; glyphs absent from the font may appear as replacement boxes. Damaged inputs
produce a failure badge/reason; a missing optional helper leaves the regular icon.
No font is installed into the user's font collection.

The requested model and art-project families are assessed separately; the font
provider does not register their MIME types or claim to render them:

| Family | Current coverage and backend boundary |
| --- | --- |
| glTF / GLB | No provider. A future glTF importer must supply bounded mesh/material decoding and handle external buffers/textures without opening arbitrary paths. |
| STL, PLY, OBJ | No provider. Mesh import needs a separate isolated renderer; OBJ material/texture references need explicit resource rules. No Assimp dependency is currently linked. |
| STEP | No provider. CAD tessellation needs a dedicated backend such as Open CASCADE, bounded independently of triangle-mesh formats. |
| FBX | No provider. An importer and renderer would need version-specific samples and licensing review; it cannot be inferred from glTF support. |
| Blender | No provider. No Blender process or project scripts are launched. A saved embedded preview could be supported separately from scene rendering. |
| CLIP, MDP, SAI2 | No provider. No generic image decoder is assumed to understand these proprietary project containers. Each needs a tested bounded embedded-preview reader or supported isolated renderer. |
| Fonts | FreeType helper described above: scalable TTF/OTF and first collection face only; no font editing or installation. |

Unsupported formats continue to show file information and can be opened in an
external application. There is no automatic dependency download or external
renderer execution from the preview path.

### Audio and video playback

With `rummage-media` installed, Quick Look opens local audio/video files paused.
Click **Play** or press **P** to start or pause. Click the progress bar or press
**J/L** to seek backward/forward five seconds; **Home** returns to the start.
**+/-** changes volume in ten-percent steps, including mute at zero. The current
time, duration and volume remain visible. **Space/Escape** closes Quick Look;
arrow keys still move to another file. Closing, changing files and pausing stop
audio output; closing or changing files also terminates and reaps the decoder.
Playback does not start for neighbouring files prepared as still previews.

Audio uses the session's default PulseAudio output, including a PipeWire server
with PulseAudio compatibility. No audio device is required for silent video.
When the server/output helper is unavailable, an inline message appears and video
continues silently. A missing decoder, unsupported codec, damaged file, failed
seek or decoding timeout produces an inline message; the file can still be
opened in its usual application.

The optional media package contains separate `rummage-playback` and
`rummage-audio` programs. FFmpeg demuxing/decoding stays in the same seccomp and
Landlock cage used for thumbnails. It reads one already-open regular file,
refuses external paths/URLs, and sends only validated, sealed RGBA/PCM buffers.
The audio program receives raw stereo 48 kHz signed 16-bit samples and owns the
sound-server connection; it does not parse media. The main application links
neither FFmpeg nor PulseAudio. Playback uses a dedicated worker and decoder,
independent of the thumbnail pool.

Playback supports the installed FFmpeg decoders in MP4/MOV, Matroska/WebM, AVI,
MPEG/transport streams, Ogg, FLV, ASF, MJPEG, YUV4MPEG, MP3, FLAC, WAV and AAC.
Playlists, network URLs, external-resource containers and DRM are not supported.
Preview output is limited to a 1280×720 box (upright after rotation), updates at
20 frames per second, and uses software decoding. Each reply contains at most
one second of stereo PCM; the UI keeps only the latest frame. Decoder reordering
queues are capped at 32 frames and 64 MiB. Inputs are limited
to 32 million pixels per decoded frame, 32 audio channels and 384 kHz, with a
four-second/128 MiB read budget per step and a six-second client timeout. Files
longer than seven days, unusually sparse/interleaved streams or complex files
that exceed these bounds may fail. Seeking is approximate to decoded frames;
Quick Look is intended for brief inspection, not frame-accurate editing.
