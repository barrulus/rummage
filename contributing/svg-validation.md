# SVG renderer acceptance

[Documentation](../docs/README.md) · [Development](development.md) · [Security model](../docs/security.md)

Validate renderer changes with semantic and geometric tests, native-size visual
review, performance checks and sanitizer fuzzing. Keep captured references and
the numeric comparator unchanged: corpus percentages are diagnostic measurements,
not release gates. Record results with the relevant change rather than in this
guide. Icons and previews share the librsvg adapter in `src/svg`; do not add
custom stroke or raster approximation code to chase reference pixels.

## Required checks

- All SVG semantic, geometric, API, bounds and committed original-fixture tests
  pass. Keep exact pixels where geometry supports them. Keep the existing fixture
  comparator; do not recapture or replace references to accommodate changes.
- Full release tests, ASan/UBSan tests, and the Nix package build pass. Record any
  test-environment accommodations with the results. A final-source sanitizer fuzz
  run lasts at least 1800 seconds with no new finding, timeout or OOM.
- Total parse/render time stays within 1.5 times each preserved corpus baseline
  at 16, 48 and 128 px. Measure after competing builds and fuzzing stop.
- Review every entry in `tests/data/svg-review.tsv` at native 16, 48 and 128 px on
  light and dark backgrounds. The fixed selection includes all 17 filtered
  Adwaita icons, all three filtered hicolor icons, prior problem silhouettes,
  thin strokes, patterns, embedded images and a known sizing-contract difference.
  Do not replace entries because they look worse or have low pixel scores.

## Visual acceptance

An icon must retain its identity, overall silhouette, intended content and usable
contrast at its display size. Missing components, unexpected holes, clipping,
broken masks, misplaced images, missing patterns, unreadable outlines and broad
unexplained colour loss block acceptance. Also record rendering errors encountered
outside the fixed selection; this set is not permission to ignore known defects.

Differences in antialiasing, subpixel edge placement and fine stroke weight can be
accepted after inspection when identity, content and usability are retained.
Text support follows librsvg and the available fonts. Public aspect-ratio sizing continues
to round; the old CLI's ceiling-sized reference does not override that contract.
Enlargements help explain a difference but do not substitute for native-size review.

Record the renderer hash, selection, review result, named deviations and any open
defects. A contact sheet alone is not evidence that the review passed. If the
simpler backend has unacceptable native-size defects, report them and revisit the
renderer constraints; do not restart a custom pixel-matching loop.

## Reproduction

Inside `nix develop`, after building `rummage-svg-render`:

```sh
python3 scripts/svg_review.py --output build/svg-review
python3 scripts/svg_corpus.py --report build/svg-review/corpus.json
```

The review command verifies source hashes and requires all selected captures. It
creates a self-contained native-size comparison and a JSON rendering inventory.
Missing or changed sources make the review incomplete; they are never silently
replaced. Neither command invokes resvg. The full corpus still records every alias,
numeric failure and timing. Keep third-party sources and generated reviews local
under ignored build directories; only selection identifiers are committed.

## Hardening checks

Theme icons render in-process on the UI thread; SVG file previews use the same
librsvg adapter in the isolated helper. Shared adapter checks apply in both paths:

| Resource | Enforced limit |
| --- | --- |
| Input | 8 MiB, checked before file reads and again at the shared byte-span entry point |
| Output | At most 1024 pixels per side; aspect ratio retained, rounded dimensions |
| External resources | No base URI; only internal references and data URIs |
| Parsing and rendering | librsvg's default limits; unlimited parsing is never enabled |

Cairo's premultiplied native-endian ARGB is converted once in the shared adapter
to straight-alpha RGBA. CSS physical units use 96 DPI. Allocation exceptions
return an empty image and a failure; the square icon/tool convenience entry point
also rejects sizes outside 1..1024.

The removed custom renderer's geometry, CSS-work, raster-work, layer-storage,
embedded-image and blur budgets no longer apply. In-process icons have no
whole-process memory ceiling or rendering deadline. Preview helpers retain their
process limits, deadline and preloaded generic-font map; see
[preview security](../docs/security.md). Normal icon rendering uses the calling
thread's Pango font map, without preloading the helper's restricted font set.

Keep tests for the adapter's input/output bounds, aspect ratios, alpha conversion,
error recovery and resource isolation. Verify CSS/filter output through both
`IconTheme::render` and the file-preview loader. Keep the committed fixture
captures and comparator unchanged; document named librsvg compatibility differences
rather than emulating the deleted engine. Preserve the original regression seeds
under `tests/fuzz/svg/` and exercise them with the shared `rummage-fuzz-svg` target.

For numeric hardening, also enable `-fsanitize=float-cast-overflow` in the fuzz
build and set `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. A zero process
exit alone is insufficient when UBSan recovery is enabled: inspect diagnostics.
The final-source fuzz requirement remains at least 1800 seconds with no finding,
timeout or OOM. Keep its binary immutable, record source/binary hashes, and verify
the process has exited before measuring final corpus performance. System librsvg
and its dependencies are not sanitizer-instrumented by the C++ adapter build.
