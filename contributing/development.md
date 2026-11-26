# Development

[Documentation](../docs/README.md) · [Contributing](../CONTRIBUTING.md) · [Architecture](architecture.md)

## Build environment

Use the [installation dependency list](../docs/install.md#build-dependencies), or enter
`nix develop`. The shell supplies build dependencies, doctest, clang tools, GDB
and the optional JPEG benchmark dependency. It does not configure a build on entry.

Use separate directories for different configurations:

```sh
cmake -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dev
ctest --test-dir build-dev --output-on-failure
./build-dev/rummage ~/Pictures
```

The GUI requires a Wayland session; many tests use offscreen rendering and require
working EGL/GLES and fonts even without a compositor. CMake generates
`compile_commands.json` in the build directory. Point your editor at that file.

## CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` when unspecified for a single-config generator | Use `Debug` for local diagnosis |
| `CMAKE_INSTALL_PREFIX` | CMake default | Set explicitly for installation/portal activation |
| `RUMMAGE_BUILD_TESTS` | `ON` | Build tests if doctest is found |
| `RUMMAGE_WERROR` | `ON` | First-party warnings are errors; package builds may disable |
| `RUMMAGE_HARDENING` | `ON` | Compiler-supported stack/control-flow protection, PIE, full RELRO and libstdc++ assertions; fortify in optimized builds |
| `RUMMAGE_SANITIZER` | Empty | e.g. `address,undefined` or `thread` |
| `RUMMAGE_CLANG_TIDY` | `OFF` | Run clang-tidy while compiling |
| `RUMMAGE_MEDIA` | `ON` | Build the bundled sandboxed FFmpeg helper |
| `RUMMAGE_PDF` | `ON` | Build the bundled sandboxed PDFium helper |
| `RUMMAGE_PHOTOS` | `OFF` | Build the optional SQLite photo module |
| `RUMMAGE_FUZZ` | `OFF` | Build libFuzzer targets; requires Clang |

Hardening applies to all project targets, including the C decoder. Fortify level 3
is enabled in `Release`, `RelWithDebInfo` and `MinSizeRel` configurations, except
when `RUMMAGE_SANITIZER` or `RUMMAGE_FUZZ` is set. Debug builds keep the other
protections. `-DRUMMAGE_HARDENING=OFF` disables these project flags; Nix and native
package toolchains can still supply their own hardening flags independently.

Tests require the `shared-mime-info` pkg-config file and MIME database at configure
time; the Nix development shell supplies both. Test processes use that database
instead of inheriting the host's `XDG_DATA_DIRS`.

Tests are silently absent from a later `ctest` run if configure skipped them;
read the configure notice and check `ctest --test-dir build-dev -N`.

## Tests

Most tests are doctest cases in `build-dev/tests/rummage-tests`, registered as a main CTest test plus a separate memory-test process. Filter cases at the executable level:

```sh
./build-dev/tests/rummage-tests --test-case='picker:*'
./build-dev/tests/rummage-tests --test-case='actions:*'
./build-dev/tests/rummage-tests --test-case='shortcuts:*'
./build-dev/tests/rummage-tests --test-case='the example config parses clean and matches the defaults'
```

Use `--list-test-cases` to find names rather than relying on a fixed count. The
portal and notification integration executables use a private D-Bus session;
CTest registers them when `dbus-run-session` is found:

```sh
ctest --test-dir build-dev -R 'file-picker-portal|action-notifications' --output-on-failure
```

Portal tests drive the real backend with a fake child. They do not replace a
manual check of an application's Open/Save dialog on the intended desktop.

The Nix test derivation supplies fonts, shared MIME data, software rendering and
archive tools. It runs CTest, including portal/notification integrations; the
allocator-sensitive test runs in a fresh process. Direct dev-shell tests use the
host environment and can differ. The normal package omits tests; `nix flake check`
checks a separate test build with media and PDF enabled, the installed package and the module.
See [CI/package validation](ci-packaging.md) for exact checks and distribution builds.

```sh
nix flake check
```

This checks the tests, installed package and module defined in the flake. In a Git-backed flake,
untracked source files can be excluded from the input; ensure intended source
files are included before interpreting a Nix build as verification of your work.

## Sanitizers and static analysis

```sh
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DRUMMAGE_SANITIZER=address,undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DRUMMAGE_SANITIZER=thread
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

Keep ASan/UBSan and TSan in separate builds. Sanitizer runtimes change memory
requirements and helper sandbox allowances; see [security](../docs/security.md).
Known third-party leak suppressions live in
`src/core/src/lsan_suppressions.cpp`. Record suppressions and environment workarounds
when reporting results.

Enable build-time analysis with `-DRUMMAGE_CLANG_TIDY=ON`. Check formatting with
`scripts/format.sh --check`. Neither formatter nor tidy covers documentation;
use `python3 scripts/check-docs.py` for its mechanical checks.

## Fuzzing

```sh
CC=clang CXX=clang++ cmake -B build-fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRUMMAGE_FUZZ=ON \
  -DRUMMAGE_WERROR=OFF -DRUMMAGE_BUILD_TESTS=OFF
cmake --build build-fuzz
mkdir -p /tmp/rummage-fuzz-tiff-corpus
./build-fuzz/tests/fuzz/rummage-fuzz-tiff /tmp/rummage-fuzz-tiff-corpus \
  tests/data/image/tiff -max_total_time=60
```

The example is a smoke run, not a release qualification. Targets are `tiff`,
`camera`, `picture`, `dng`, `ljpeg`, `text` and `svg` with the
`rummage-fuzz-` prefix. Keep generated corpus files separate from committed seeds;
preserve crash inputs and reproduce them in the same instrumented build.
Camera corpora may require local samples; SVG corpus references are not all shipped.
SVG changes have the longer fuzzing and visual requirements in
[their validation policy](svg-validation.md).

## Diagnostic tools

These CMake targets are development executables and are not installed by the
normal application install. Run them from the build directory (prefix examples
with `./build-dev/`).

| Tool | Purpose |
| --- | --- |
| `rummage-snapshot` | Offscreen PPM renders of the scene/browser, interaction fixtures and scrolling measurements |
| `rummage-svg-render` | Render one SVG to PNG; optional reference comparison |
| `rummage-bench-start` | Time startup of a specified command |
| `rummage-bench-dir` | Directory-loading benchmark and fixture generation |
| `rummage-bench-find` | Recursive fuzzy-search measurements |
| `rummage-mime` | Print file MIME classification |
| `rummage-watch` | Observe live directory-model changes |
| `rummage-decode` | Decode/inspect images and raw previews with timings |
| `rummage-thumb` | Exercise the thumbnail cache |
| `rummage-open` | Inspect default-application resolution; use `--dry-run` to avoid launching |
| `rummage-transfer` | Exercise real copy/move operations on disposable fixtures; `--threads 1` through `--threads 64` selects copy concurrency |
| `rummage-bench-jpeg` | Optional wuffs/TurboJPEG comparison |
| `tests/rummage-bench-previews` | Optional exiv2/reference comparison |

Examples:

```sh
./build-dev/rummage-snapshot --columns ~/Documents /tmp/rummage.ppm 960 640 1
./build-dev/rummage-snapshot --columns ~/Documents --picker save /tmp/picker.ppm 960 640 1
./build-dev/rummage-bench-start -n 5 -- ./build-dev/rummage ~/Pictures
./build-dev/rummage-open --dry-run ~/Documents/notes.txt
```

Not every development tool implements `--help`; inspect its `src/tools/*.cpp`
usage string before adding options. Snapshot output checks pixels, not compositor
input or desktop interoperability. Record dimensions, scale, theme and fixtures.

`rummage-bench-start` uses `RUMMAGE_STARTUP_NS` and `RUMMAGE_STARTUP_QUIT` to collect
application startup marks. Prefer the wrapper to setting them by hand. Keep
timings separate from competing builds/fuzz runs and label warm/cold caches,
hardware versus software rendering, and on-screen versus offscreen measurements.

## Motion validation and profiling

Run `build-dev/tests/rummage-tests --test-case='motion:*'` for deterministic
scheduler, config, portal-policy, browser and offscreen shader tests. An EGL/GLES
context and fonts are required. With `RUMMAGE_MOTION_FRAMES=/tmp/motion-frames`
(create that directory first), the shader tests also write PPM stills for review.

Send `SIGUSR1` to a running Rummage process for capture bytes, allocation/pass/
fallback counts and last/maximum CPU frame-submission time. These timings exclude
GPU completion and compositor presentation; they are not frame-pacing or integrated
GPU performance claims. Compare hardware, scale, dimensions, theme, build mode and
revision when profiling. The texture counters exclude driver overhead.

`rummage-snapshot --columns DIR --memory /tmp/preview.ppm` also reports motion
texture bytes, allocation count and pass count. Capture textures are released once
all effects finish; the compiled shader stays available for the next effect.
Allocator and driver caches can retain freed storage, so compare live allocations
as well as process RSS when investigating preview memory.

## Preview memory

Use the [paired performance harness](performance.md) to measure live preview
processes under the same rendering conditions.

The worker pool assigns spaced jobs to the most recently idle worker. This lets
sequential previews reuse that thread's allocator arena instead of retaining a
preview-sized free block on every worker. Bursts still wake additional workers;
queued work keeps its visible/nearby/prefetch priority order. When investigating
retention, distinguish live allocations from freed blocks held by allocator
arenas, and check both repeated navigation and concurrent work.

## Maintaining shell completions

When public CLI usage changes, update all three files in `data/completions`.
`python3 -m unittest discover -s scripts/ci/tests -p test_completions.py` compares
public option coverage with the usage string and exercises Bash/Fish completion
results plus Zsh argument specification and context parsing. Local tests skip
shells that are unavailable; the lint preflight supplies all three shells from
the locked Nix input. Zsh context tests do not substitute for an interactive ZLE
check after changes to its completion presentation. The native package smoke
checks also require the three installed completion files.

The bounded [sanitizer and fuzz gate](ci-packaging.md#sanitizer-and-fuzz-gate)
provides repeatable instrumented checks and retains failure evidence.

### Focused filesystem matrix

Provision a disposable mount with fixture data only, then run the focused
transfer, rename/create, compression/extraction and Undo checks against it:

```sh
python3 scripts/ci/filesystem-matrix.py --binary build-dev/tests/rummage-tests \
  --target /path/to/disposable/mount --output /tmp/filesystem-results \
  --backend 'protocol, client and server versions; mount options' --expected-fstype nfs4
```

The runner does not mount or configure services. It sets `TMPDIR` for fixtures,
records separate test logs, exit status and leftover paths, and preserves any
remaining fixture data for review. Capability differences (timestamps, xattrs,
links, permissions or device write support) can fail POSIX-oriented assertions;
inspect the exact assertion and operation outcome before classifying a product
bug. Run as an ordinary user for permission checks. Include restrictive server
policies, cancellation, history disposal and exit cleanup in backend reports.
