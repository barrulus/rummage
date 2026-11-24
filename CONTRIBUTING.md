# Contributing

[Documentation](docs/README.md) · [Development](contributing/development.md) · [Architecture](contributing/architecture.md) · [Performance measurements](contributing/performance.md)

Start with the code and tests for the affected module. rummage is a C++20 Wayland
application with its own UI; changes should preserve responsive browsing, clear
file-operation behavior and preview isolation. Keep unrelated work separate and
describe visible behavior and limitations in the change.

## Build and verify

The [development guide](contributing/development.md) gives the complete dependency,
configuration and test workflow. A typical local start is:

```sh
nix develop
cmake -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dev
ctest --test-dir build-dev --output-on-failure
```

Use the repository's `.clang-format` and `.clang-tidy` settings. Check formatting
with `scripts/format.sh --check`; `scripts/format.sh` rewrites tracked C++ files,
so avoid running it across unrelated unfinished edits. New untracked files need
formatting explicitly until they are tracked.

For behavioral changes, add or update focused tests that demonstrate the outcome.
Run the relevant tests, then the appropriate broader checks. Renderer changes
also need visual inspection; a screenshot does not establish keyboard, clipboard
or portal behavior. Parser/sandbox changes need sanitizer and adversarial-input
coverage. SVG changes follow the existing
[SVG validation policy](contributing/svg-validation.md).

## Before proposing a change

- Run `python3 scripts/ci/preflight.py lint` while editing. After committing, run
  `python3 scripts/ci/preflight.py lint tests` for the shared CI tooling and full test gate.
  Add the `nix` target for Nix package and installed checks.
  Dependency, CMake, packaging, helper or install-layout changes also require
  `python3 scripts/ci/preflight.py all` (Nix and Docker). A Nix pass alone does not
  verify native packages. See [preflight](contributing/ci-packaging.md#local-preflight).
- CI runs automatically on pushes/merges to `main`; PRs do not trigger it. Before merging,
  dispatch the workflow manually for the branch and require the test gate and every
  package job to pass on the current PR revision;
  update/retest against current `main` when concurrent changes affect dependencies
  or packaging. Do not merge a known failing check as an unrelated failure.
- State the problem and resulting user-visible behavior.
- Explain relevant tradeoffs, dependency changes and compatibility effects.
- List checks actually run and any failures or environment limits.
- Update the relevant guide, configuration example and tests alongside behavior.
- For measured improvements, record revision, machine, build mode and method.

Do not call a test suite successful if doctest was absent and CMake skipped it.
Do not promote old benchmark numbers to measurements of a new build.

## Reports

For bugs, include steps to reproduce, expected/actual behavior, `rummage --about`,
and relevant stderr output. Say whether the binary is a release package, a Nix
build or a locally modified checkout. For preview bugs, identify the format and
provide a shareable minimal sample when possible. Remove private paths/content
from anything you publish. See [troubleshooting](docs/troubleshooting.md).

## Documentation

Documentation is Markdown rendered by the repository browser; no site generator
or publishing step is required. [docs/README.md](docs/README.md) is the application
documentation index.

Keep `docs/` limited to application documentation: installation, usage,
configuration, integrations, supported formats, troubleshooting, security and
user-visible release notes. Maintained contributor procedures belong in
`contributing/`, linked from this guide. Do not add investigation reports, task
briefs, agent handoffs, development diaries, benchmark result logs or implementation
plans to either documentation collection. Keep temporary work notes local and
record review-specific evidence in the relevant issue or pull request.

Keep the root README as an introduction and entry point. Put procedures in topic
guides and detailed option lists in references. Add a link from the documentation
index for a new page; use relative links and descriptive text. Prefer stable
source-file links over line numbers that drift.

Describe current behavior in the guides. Release notes should describe
user-visible changes, without implementation diaries or unqualified performance
claims. Keep contributor instructions reusable and independent of past task IDs,
private notes and local build artifacts.

When changing configuration, update defaults/ranges in
[configuration.md](docs/configuration.md), recipes in
[config.example.toml](docs/config.example.toml), and the affected topic guide.
Check TOML examples and `--check-config`; the existing config test verifies that
the active example matches defaults. When changing commands or shortcuts, update
their references from `src/app/main.cpp` and `src/views/src/shortcuts.cpp`.

Run `python3 scripts/check-docs.py` (Python 3.11+) to check local links, heading
anchors and TOML code blocks in the public documentation. It does not check external URLs or
prove that instructions match runtime behavior; source review and appropriate
focused checks are still needed.

## Project history and credits

Contributions should be reviewable on their behavior and evidence regardless of
the tools used. Bundled material and licenses are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
