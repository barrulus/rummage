# Paired performance measurements

Use `scripts/performance/` to compare live Rummage and Nordend processes on the
same Linux host. This procedure covers executable size, direct and loaded
libraries, startup, idle memory and CPU, scrolling, image and text previews,
input delivery, and a synced 1 GB copy. Keep result logs outside the source tree.

Build both applications in Release mode with the same compiler and dependencies.
Record optional feature settings; additional Rummage features need not have a
Nordend equivalent. Build each application's main executable, preview helper and
`*-transfer` tool. Do not run builds or tests while collecting measurements.

## Prepare telemetry

The live input tests need a small probe in each application's main loop. The
preparation script compiles a temporary copy of `main.cpp` and links it against
the existing build's libraries. It does not edit either checkout or overwrite the
original binaries. The probes are identical for both applications. Their per-frame trace writes
add overhead; the results do not subtract a separately calibrated probe cost. Source anchors
are checked so an upstream change fails visibly instead of silently dropping a
measurement. Run preparation inside the original build's development shell.

```sh
python3 scripts/performance/instrument.py \
  --source /path/to/rummage --build /path/to/rummage/build-perf \
  --prefix rummage --out /tmp/performance-bin/rummage
python3 scripts/performance/instrument.py \
  --source /path/to/nordend --build /path/to/nordend/build-perf \
  --prefix nordend --out /tmp/performance-bin/nordend
```

Virtual input uses Umbriel's existing test client and generated Wayland protocol
files, with monotonic send timestamps added in a temporary source copy. It
requires Wayland, xkbcommon and a C++23 compiler with `std::print` support.

```sh
python3 scripts/performance/build_input.py \
  --umbriel /path/to/umbriel --build /path/to/umbriel/build-debug \
  --out /tmp/performance-input
```

## Run on an isolated display

The display runner requires an installed Umbriel compositor and access to a GPU
render node. Choose the node using `/sys/class/drm/renderD*/device/driver`;
render-node numbers are machine-specific. Pin the graphics vendor when needed
and use `--expect-renderer` to reject a software fallback. The example below is
for an Intel/Mesa machine; adapt the renderer and GLVND JSON path for the host.

```sh
python3 scripts/performance/display.py \
  --directory /tmp/performance-display \
  --render-device /dev/dri/renderD129 \
  --egl-vendor /run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json -- \
  python3 scripts/performance/run.py \
  --runtime '{runtime}' --input /tmp/performance-input/input \
  --rummage-source /path/to/rummage --rummage-build /path/to/rummage/build-perf \
  --rummage-probe /tmp/performance-bin/rummage/rummage \
  --nordend-source /path/to/nordend --nordend-build /path/to/nordend/build-perf \
  --nordend-probe /tmp/performance-bin/nordend/nordend \
  --out /tmp/performance-results --fixtures /tmp/performance-fixtures \
  --copy-root /path/to/disk-backed/performance-copy \
  --runs 3 --startup-runs 9 --idle-seconds 10 --scroll-seconds 30 \
  --expect-renderer 'Mesa Intel'
python3 scripts/performance/report.py /tmp/performance-results/results.json
```

Output directories must be new. Fixtures are reusable and their modification
times stay stable. Use a disk-backed copy directory with at least 2 GB free;
copying on tmpfs measures memory-backed storage. The copy fixture is retained,
but verified destination copies are removed after each trial. Application
configuration, cache, state and data roots are isolated; `HOME` is unchanged.
A shared per-application cache is warmed before measured trials. The compositor
uses its own runtime and explicit IPC socket, disables autostart and Xwayland,
and cannot deliver the benchmark input to the desktop compositor. It is stopped
when the command finishes or fails.

For a quick infrastructure check, use `--phases live --live-fixtures files20
--runs 1 --idle-seconds 2`. Such a check is not a comparative performance result.
`--phases startup live copy` selects measurement groups independently.
`--phases idle` repeats only the idle CPU measurements.

True cold startup is optional through `--cold-cache-hook COMMAND`, run before
each measured cold launch. Supply a cache-reset procedure only on a disposable
benchmark host. A new process, new application cache or `posix_fadvise` on one
executable does not establish a system-wide cold filesystem cache. The report
must retain the exact hook and distinguish its cold samples from warm starts.

## Interpret measurements

| Original comparison metric | Harness measurement |
|---|---|
| Binary size | Stripped copy of the original executable; unstripped size and SHA-256 also recorded |
| Direct / loaded libraries | ELF `DT_NEEDED` entries / distinct mapped `.so` paths in the live idle process |
| Warm startup, 20 and 10,000 files | Original executable's `folder-shown` frame callback; alternating app order, discarded warm-up pair |
| Cold startup, 20 files | Same endpoint after the supplied cold-cache hook; explicitly unmeasured without a hook |
| Idle memory, 20 / 10,000 files / 30 large images | Live process and helper tree after settling; three samples per process |
| Idle CPU | CPU time during an idle interval, normalized to one core; sampling resolution recorded |
| Scrolling median / p95 / worst / late frames | Frame callback gaps during continuous virtual pointer input; late means >1.5× output frame budget |
| Memory after scrolling | Same process after animation settles |
| PNG / C preview latency | Virtual key send to callback for the expected selection; loaded and animation-complete endpoints are separate |
| PNG missed keys | Delivery failures, unchanged selections and missing settled results are reported separately |
| Memory after PNG / C preview | First selection, after 28 transitions, and Quick Look are separate samples |
| Copy 1 GB including sync | Application transfer engine and `cp`, then destination file and directory fsync; hash verified outside timing |

Memory values are bytes in JSON and decimal MB in the report. Main-process RSS
counts shared resident pages in full. PSS apportions them; private memory sums
private clean and dirty pages. Tree PSS includes live helpers without counting
shared mappings in full for each process. Summed tree RSS does double-count
shared mappings and is retained only as a raw diagnostic. Compositor memory and
some GPU allocations are outside these figures. A software-rendered offscreen
snapshot's RSS is a different workload and must never replace a live-window
number from another machine.

The image fixture has 30 distinct 4000×3000 RGB PNG files with identical pixels
and differing metadata. This exercises distinct cache keys while keeping image
content complexity fixed. Text fixtures have 30 files each, containing either
30 or 5,000 synthetic C lines; exact sizes and hashes are recorded. Nordend's
8 KiB / 40-line sidebar text limit and Rummage's scrollable text limit differ, so retain both
sidebar and Quick Look measurements rather than assuming equal work.

Frame callbacks report compositor pacing, not physical display presentation.
The headless display cannot establish input-to-photon latency. Preview timing
separates the first loaded callback from the callback after animation finishes;
an animation tail is not additional decoding time. Preview settlement
is an application-state endpoint, not a pixel-stability heuristic; do not compare
it numerically with another report unless that report uses the same endpoint.
The selection checks apply to column mode. Input is paced at 400 ms, so this does
not establish behavior under rapid key-repeat or burst input.

Idle CPU uses the high-resolution POSIX process CPU clock, including all
main-process threads and excluding helpers. This avoids the 0.1 percentage-point
quantization of 100 Hz tick accounting over a 10-second interval. Repeat idle
intervals to distinguish occasional work from sustained CPU use; the clock’s
nominal resolution does not establish statistical accuracy. Frame-gap worst
values and late-frame counts also depend on trial duration and background load.
Keep raw distributions, trial order and sample counts alongside medians.

Run the metric interpretation tests without a display:

```sh
python3 -m unittest discover -s scripts/performance -p 'test_*.py' -v
```
