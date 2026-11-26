# CI and package validation

[Documentation](../docs/README.md) · [Installation](../docs/install.md) · [Development](development.md)

The fork's [workflow](../.github/workflows/ci.yml) runs automatically only on pushes
to `main`, including merges into `main`. Opening or updating a pull request does not
start CI. To validate a branch before merging, use **Actions → Build, test and package
→ Run workflow** and select the branch, or run `gh workflow run ci.yml --ref BRANCH`.
Manual dispatch is available for every branch. It currently validates **x86_64 Linux with glibc**. The flake still
exposes aarch64 packages, but this workflow does not claim to build or run them. Debian 13,
Fedora 45, Arch Linux and openSUSE Tumbleweed are also built as native packages. Void musl, older Debian/Ubuntu
releases, other RPM distributions and signed/stable releases are outside the validated scope.

All package jobs wait for the **Tests and lint** job to pass. This includes manual
Tumbleweed-only runs. A test or lint failure skips Nix, Void and every native package
build. After the gate passes, package jobs run in parallel and retain their separate
installation and smoke checks.

Successful runs can take longer because tests no longer overlap packaging. The
additional wall time depends on caching and which package job was previously slowest;
it is bounded by the test stage plus job/artifact overhead. Failed tests avoid the
packaging work entirely. The Nix package job imports the successful test result from
the gate instead of compiling and running the suite again.

## Local preflight

Use the shared runner instead of reconstructing a subset of CI commands:

```sh
# Fast workflow/script lint, CI tooling tests and documentation checks; allows edits.
python3 scripts/ci/preflight.py lint
# Commit intended files first, then run the test gate without packaging.
python3 scripts/ci/preflight.py lint tests
# Tests first, then Nix package and installed checks.
python3 scripts/ci/preflight.py lint tests nix
# All six build environments, including fresh native-package installation.
python3 scripts/ci/preflight.py all
# Reproduce one native job, or select several explicitly.
python3 scripts/ci/preflight.py void
python3 scripts/ci/preflight.py deb rpm arch tumbleweed
```

With no targets, the runner selects `all`. Nix supplies the lint tools from the
locked nixpkgs input. Native targets require Nix to assemble pinned renderer sources, a working Docker daemon and the host
tools listed below. Build targets reject dirty or untracked files, because a Nix
flake can omit untracked source and the native builders export `git archive HEAD`.
Lint alone accepts unfinished edits and labels its report accordingly. Its Nix shell
includes `bashInteractive` because the completion tests need the `compgen` and
`complete` builtins, which minimal Bash builds can omit.

Every invocation creates a new `/tmp/rummage-preflight-*` directory with per-target
logs and `summary.json`, recording the commit, requested targets and results.
Use `--output /path/to/new-directory` to retain them elsewhere outside the checkout.
Failures stop the run; unrun targets are absent from the results, never reported as
passing. Selected targets are explicitly reported as partial coverage. No target
publishes downloads or changes repository settings.

Run lint and Nix checks before proposing code changes. Changes to dependencies,
CMake, optional helpers, package recipes or installed resources require the full
matrix: a successful Nix build cannot detect a missing Debian/Arch/RPM/Void build
dependency. Test asynchronous behavior by waiting for the actual result, with a
timeout; a directory scan completing does not mean its preview worker completed.
Use focused repeated tests when changing that behavior.

Before merge, all required jobs must pass for the current PR revision and relevant
changes on `main`. Configure the required checks listed under
[GitHub setup](#github-setup-and-scope); local preflight does not enforce branch
protection. If a shared dependency failure affects several branches, fix it on
`main` and refresh the branches before continuing feature merges.

Void's pinned recipe revision and rolling binary repository can diverge. When
updating `VOID_PACKAGES_REV`, use a reviewed revision and run the complete Void
build and fresh-install check. The pinned container image does not freeze the
dependency repository.

Tumbleweed's build and fresh-install containers use `https://downloadcontent.opensuse.org`
for the standard openSUSE repositories. This bypasses geographic mirror redirects,
which can serve repository metadata before the referenced files reach the selected
mirror. Signature verification remains enabled. The repositories still roll forward;
this is not a snapshot pin, and origin outages can still fail the job.

## Nix

The **Tests and lint** job runs the workflow/script/documentation checks and then
builds only `checks.x86_64-linux.tests`. That derivation compiles the application and
helpers needed by the complete test suite, including media and PDF, but replaces
installation with a success marker. It produces no installable rummage packages.
Dependencies such as the PDF renderer may still need to compile on a cold cache.

The **Nix packages and installed checks** job runs after the gate succeeds:

```sh
nix flake check --print-build-logs
nix build --no-link --print-build-logs \
  '.#rummage^*' '.#rummage-with-photos^*'
```

Checks include:

- `tests`: build the complete unit suite and the private D-Bus file-picker and notification
  integration tests, with deterministic fonts, a MIME database, software rendering and
  archive tools. Allocator-sensitive tests run in a separate process, not after the rest
  of the suite has changed glibc's allocation thresholds.
- `smoke`: inspect the actual installed package, validate the installed example config and
  feature syntax, check desktop/portal files and their paths, then render a window under
  headless Weston. Merely surviving startup is insufficient: the Wayland log must show
  the application attaching a buffer.
- `nixos-module`: evaluate module enablement, portal selection, coexistence with other
  portal backends, and invalid configurations.

The ordinary package remains a release build without test executables. The separate
`checks.<system>.tests` derivation enables tests and exports only their success marker.
The gate uploads that result as a small binary-cache artifact; the Nix package job
imports the expected output evaluated from the same checkout before running flake
checks. This also avoids repeating the suite on fork runs without Cachix upload access. Tests that probe optional kernel capabilities retain their
existing capability checks.

All runs read the public `rummage-fm` Cachix cache. Runs in `barrulus/rummage`
upload completed derivations as they finish, using the
cache-specific `CACHIX_AUTH_TOKEN` and Cachix-managed signing. A later test failure
does not discard successful dependency builds. Failed derivations are not cached.
Being in the cache does not mean a revision passed the complete release matrix;
use the `development` tag for a validated snapshot.

Runs in other forks receive no Cachix token from this repository. They restore and
save the Nix store using GitHub's branch-scoped cache, including after a test failure,
so later manual runs on that branch can reuse
completed work. Cache keys group matching architecture, `flake.lock` and Nix recipes;
Nix still rebuilds outputs whose inputs changed. GitHub cache eviction can cause a
cold build. Cancelling a run may interrupt cache saving.

Successful manual branch runs retain the test result separately and all outputs of the four
installable variants and three checks as binary-cache artifacts for 14 days.
The test gate restores only the small test-result artifact on an identical-tree
merge; it does not download the full package closure. On an identical-tree merge, main
imports roots evaluated from its own flake, runs the usual Nix preflight (which can
reuse those successful derivations), and explicitly publishes imported outputs to
Cachix. Bundled preview helpers, the photo output and the custom PDFium runtime closure are included.
The test derivation alone would not provide the ordinary installable packages.

Local preflight never uploads. Only x86_64-linux is populated by this workflow.
See [user setup](../docs/install.md#binary-cache) and [merge reuse](#reuse-at-merge).

To run just one check locally:

```sh
nix build .#checks.x86_64-linux.tests --no-link --print-build-logs
nix build .#checks.x86_64-linux.smoke --no-link --print-build-logs
nix build .#checks.x86_64-linux.nixos-module --no-link --print-build-logs
```

To share the Nix version, send the fork reference. With flakes enabled, the recipient can
run it directly or install it into their profile:

```sh
nix run github:barrulus/rummage
nix profile install github:barrulus/rummage
```

Append `/COMMIT_SHA` to that reference to select an exact revision. This fetches the source
and builds rummage if a matching binary is not cached. Cachix supplies the runtime
closure; copying only the executable omits its `/nix/store` dependencies.

## Void Linux

The `Void glibc package and clean install` job uses Docker and Void's `xbps-src`. It packages
**the exact committed checkout**, either the main push or manually selected branch head.
It does not fetch upstream's v0.5.0 and mistake that build for a test of this fork.
The template uses Void's `libassimp-devel` for model previews and `libvterm-devel`
for the embedded terminal.

From a clean, committed checkout, with Nix, Docker, Git, Bash, Python 3, tar/gzip tools and
coreutils available:

```sh
bash scripts/ci/void.sh /tmp/rummage-void-package
```

Choose a new or empty output directory outside the checkout. Uncommitted changes are rejected because
`git archive HEAD` must match the revision recorded with the artifact. `CI_JOBS` sets build
parallelism (default: 2).

The script:

1. Archives the checkout together with the pinned PDFium source bundle and records its commit, commit timestamp, application version,
   and SHA-256 in `input/source.json`.
2. Starts a disposable Void container. Inside it, `xbps-src` uses Void's container-only
   `ethereal` build mode; no privileged Docker container or host chroot is required.
3. Copies the checked-in Void recipe, changing only source/version metadata to use the
   verified checkout archive. The snapshot version is the application version followed by
   the UTC commit timestamp. The source is preseeded in xbps-src's checksum-verified cache;
   a cache miss fails rather than falling back to an unrelated release.
4. Builds the `.xbps`, then starts a **fresh container** and installs that package through
   XBPS. The build container's compiler and development dependencies cannot hide missing
   package dependencies. The Void recipe's unit tests remain disabled; the complete suite
   is exercised by the Tests and lint gate.
5. Checks package integrity and linked libraries, validates installed resources and config,
   then installs the compositor/test fixtures and performs the headless Wayland launch.

To share a Void build, download its artifact ZIP from a successful GitHub Actions run and
send the ZIP. The recipient should extract it, enter `void-package/`, and keep the entire `packages/` directory
together, including the repository index.

The artifact contains the `.xbps` and repository index in `packages/`, the effective
template, `source.json`, and the workflow build log. On a Void machine, install a downloaded
snapshot with:

```sh
sudo xbps-install -R /path/to/artifact/packages rummage
```

These are unsigned development snapshots, not an official Void repository or published fork
release. Their package version distinguishes them from upstream v0.5.0; the application
still reports its CMake version. `source.json` identifies the exact source commit.

The checked-in [checkout template](../packaging/void/rummage/template) contains
placeholder source identity. The builder supplies the exact archive and checksum;
it does not fetch an upstream release. A future stable fork release needs its own
tag, version and verified source identity.

The container digest and void-packages commit are pinned in
[`scripts/ci/void-env`](../scripts/ci/void-env). Void's binary repositories are rolling, so
those dependencies can still change; this is repeatable build tooling, not a bit-for-bit
reproducibility claim. Update the pins deliberately and rerun both build and clean-install
checks. Never run `void-build.sh` or `void-install.sh` directly on a host; `void.sh` supplies
the disposable containers.

## Debian and Fedora

The `Debian 13 package and clean install` and `Fedora 45 package and clean install` jobs
build native **amd64 DEB** and **x86_64 RPM** snapshots. These target Debian 13 (trixie) and
Fedora 45 specifically; the file extension alone does not imply compatibility with Ubuntu,
older Debian, RHEL or other RPM systems. rummage requires sdbus-c++ 2 or newer.

From a clean committed checkout, with the same host tools as the Void builder:

```sh
bash scripts/ci/native.sh deb /tmp/rummage-deb-package
bash scripts/ci/native.sh rpm /tmp/rummage-rpm-package
```

`CI_JOBS` controls parallelism (default 2). Each output directory must be empty and outside
the checkout. The shared source exporter records the exact commit and archive SHA-256.
Debian uses [`packaging/debian`](../packaging/debian) with `dpkg-buildpackage`; Fedora uses
[`packaging/rpm/rummage.spec`](../packaging/rpm/rummage.spec) with `rpmbuild`. Runtime library
dependencies are generated by the native packaging tools. MIME data and the icon theme are
explicit dependencies. Both packages include bundled third-party license notices.

The build runs in a disposable container, followed by installation through apt or dnf in a
fresh container. Package integrity and shared libraries are checked before installing any
test fixtures. The common installed-resource checks and headless Weston render check then
run against `/usr/bin/rummage`. Unit tests run in the Tests and lint gate; these two jobs test
the native build and installed package. No privileged container is needed.

The artifacts contain binary packages (plus Debian build metadata or Fedora source/debug
RPMs), the effective recipe, the exact source archive and `source.json`, and the build log.
Download and unpack the matching artifact from the workflow run, then enter its
`native-package/` directory. On the matching distro:

```sh
# Debian 13: choose the main package, not the optional debug-symbol package.
sudo apt install ./packages/rummage_*.deb
# Fedora 45: choose the main binary RPM, not the source/debug RPMs.
sudo dnf install ./packages/rummage-[0-9]*.x86_64.rpm
```

Snapshots are unsigned development packages, not an apt/dnf repository or official release.
Their versions include the UTC source commit timestamp and abbreviated hash; the application
continues to report its CMake version. A later commit produces a newer snapshot when the
commit timestamp increases. Container images are pinned in
[`scripts/ci/native-env`](../scripts/ci/native-env), but apt/dnf repositories still receive
updates, so builds are not claimed to be bit-for-bit reproducible. Run `native.sh` rather
than invoking `native-build.sh` or `native-install.sh` on the host.

### Debian source packages and copyright

The Debian builder creates an unsigned **3.0 (quilt)** source package before compiling:
`.dsc`, `.orig.tar.gz`, `.debian.tar.xz`, source `.changes` and `.buildinfo`. The original
archive is the exact checkout archive, renamed without changing its bytes. The packaging
archive contains the generated Debian recipe and snapshot changelog.

CI unpacks that descriptor with `dpkg-source -x` into a separate directory and builds the
actual shipped DEB from it. That DEB then goes through the existing fresh-container install
and rendering checks. This verifies that the source package is sufficient to rebuild the
binary; it does not claim bit-for-bit binary reproducibility across changing environments.
The release preparer checks the descriptor's source/version/format, its component SHA-256
checksums and sizes, and that its original archive matches the recorded checkout SHA-256.
A missing or mismatched source package prevents publication.

The stable `rummage-debian-source.tar.gz` download bundles the versioned source-package files
together, preserving the filenames referenced by the descriptor. The individual files are
also in the Debian CI artifact's `native-package/debian-source/` directory. See [installation](../docs/install.md#development-downloads)
for extraction, dependency installation and rebuild commands. The generic `rummage-source.tar.gz`
remains the raw checkout archive, not a Debian source package or a substitute for the descriptor.

`packaging/debian/copyright` uses Debian's machine-readable copyright format and is parsed
strictly with python-debian during CI. Its `Files` entries refer to the **source tree**.
Wuffs decoding code, the fzy matcher and subset Tabler icon fonts are compiled into the
binaries; their original source files are not installed separately. The Noctalia palette
values are documented too. These notices describe bundled components already in rummage.

## Arch Linux and openSUSE Tumbleweed

These are separate **x86_64 rolling-distribution** targets. Arch uses the
[`PKGBUILD`](../packaging/arch/PKGBUILD) with `makepkg`; Tumbleweed builds the shared
[RPM recipe](../packaging/rpm/rummage.spec) using openSUSE dependencies and RPM macros.
Neither the Fedora RPM nor the Tumbleweed RPM is claimed to support Leap, RHEL or other
RPM distributions. Ubuntu is not a CI target.

```sh
bash scripts/ci/native.sh arch /tmp/rummage-arch-package
bash scripts/ci/native.sh tumbleweed /tmp/rummage-tumbleweed-package
```

Both builders use pinned base images and fully update the disposable container first to
avoid partial upgrades against rolling repositories. Package repositories still change over
time. Arch runs `makepkg` as an ordinary build user; root is used only for installing build
dependencies. Its recipe gets the archive's verified SHA-256 and a source-derived snapshot
version. The checked-in PKGBUILD is a checkout-build template, not an AUR submission.

A second, fresh container installs the resulting package with pacman or zypper. CI checks
package integrity, shared-library dependencies, installed assets and a rendered window under
headless Weston. Test fixtures are installed after the runtime dependency checks. The complete
unit suite runs in the Tests and lint gate. Both jobs must pass before any development downloads are
published, and the release preparer requires all five native package identities to match.

The README provides stable links; [installation](../docs/install.md#development-downloads)
provides the distribution-specific commands. Arch and Tumbleweed users should
fully update their systems before installing these snapshots. These are unsigned local
packages, not configured pacman/zypper repositories; Tumbleweed's install command explicitly
allows the unsigned local RPM without changing global signature settings.

## Automatic development downloads

The README has stable links to the `development` prerelease and fixed asset names. Once all
checks succeed, `prepare-downloads.py` checks that all five package artifacts identify the
same source commit and checksum. It selects only the main installable packages, bundles the Debian source package and Void package
with its repository index, and adds the source archive, `source.json` and `SHA256SUMS`.
Release download links and the installation-guide link are copied from the marked README
section, so they stay in sync. Keep both development-downloads marker comments intact.
Preparation runs on manually selected branches too; the prepared bundle is available as a workflow artifact.

On trusted `main` runs, the publication job creates or updates **Latest development build**.
It replaces the assets, moves the `development` tag to the validated main commit, and updates the
release notes with a link to the CI run. It is explicitly a prerelease, not the repository's
latest stable release. Nix uses the same moving tag as its source reference; the Nix
job uploads binary closures separately to Cachix. No README commits are needed for subsequent builds.

Main runs are serialized without automatic cancellation, preventing overlapping publication.
Non-main manual runs cancel superseded runs on the same branch. Rerunning an older successful main workflow cannot
roll downloads back to an ancestor of the already-published commit; divergent history fails
for maintainer review. Failed build/check/preparation jobs leave the existing
downloads untouched. Asset replacement uses separate GitHub API calls, not an atomic swap:
if publication is interrupted, rerun the job to finish it. Checksums are uploaded last;
retry a download if verification fails during an update. The rolling prerelease must remain
mutable; repositories enforcing immutable releases need a different publishing strategy.

Release assets stay available until replaced or removed, independently of the 14-day CI
artifact retention. They are unsigned snapshots and must not be presented as stable releases,
an official distribution repository, or packages for untested distributions such as Leap or Ubuntu.

### Reuse at merge

A push to `main` first looks for a successful manual branch run of this workflow
whose tested checkout has the **same complete Git tree** as main. Commit IDs may
differ after a merge or squash. The lookup verifies that the tested commit is the
manual run's head, the test gate, all six package jobs and download preparation
passed, and the test-result, prepared-download and Nix-closure artifacts remain
available. It searches the latest 30 successful manual runs, excluding main runs.
Lookup errors, expired/missing artifacts or a different tree fall back to the
normal build matrix.

When a match exists, native build jobs are skipped and their already validated
packages are promoted. The Nix job imports the matching closure and resolves the
usual checks/packages without recompiling matching derivations. Download checksums
and tree equality are verified again before promotion. A corrupt or disappearing
artifact fails promotion rather than publishing unchecked files. No artifact scripts are executed, and runs in forks never receive this repository’s
release or Cachix write credentials.

Package bytes, embedded versions, source archives and their original timestamps stay
unchanged. `source.json` retains the original tested `commit` and adds `published_commit`
for the main/tag target plus the common `tree`. Release notes retain the original
successful build link and explain the promotion. This records what was actually built
instead of relabelling binaries as a new compilation.

Manual dispatch deliberately bypasses artifact promotion for a fresh matrix run
(ordinary Nix substitution still applies). Changes to workflow files or any other
tracked source prevent reuse unless that exact tree has passed. Runs created before
Nix closure artifacts were introduced need one normal main build.

## Noctalia community template

Noctalia also offers a rummage template in its catalogue,
[noctalia-dev/community-templates](https://github.com/noctalia-dev/community-templates) (`rummage/`),
maintained outside this repository. The [template workflow](../.github/workflows/noctalia-template.yml)
runs `scripts/ci/noctalia-template.py`, which compares that template with the roles of `struct Palette`
in [palette.hpp](../src/theme/include/rummage/theme/palette.hpp) and the mapping in
[data/noctalia/rummage](../data/noctalia/rummage): missing, extra or remapped roles, the `mode` line and
the output path. It also validates the explicit community colour file as TOML, rejecting malformed
tables and duplicate keys. It runs on demand and on pushes to `main` touching those
files. It has no scheduled or pull-request trigger. A changed palette role fails the
check until the community template is updated as well.

When a run on `main` finds differences, it keeps one issue, "Noctalia community template for
rummage is out of sync", open and comments when the differences change; it closes the issue once both
agree again. If both upstream files are absent, the workflow passes with a notice and leaves any
existing drift issue open: no comparison was possible. The script returns 0 for a successful match,
1 for drift, 2 for a fetch error, and 3 for an absent template (comparison skipped). To check a
template change before it is merged, point the script at its branch or a local checkout:

```sh
python3 scripts/ci/noctalia-template.py --repo <user>/community-templates --ref <branch>
python3 scripts/ci/noctalia-template.py --dir ../community-templates
```

## GitHub setup and scope

GitHub Actions must be enabled on the fork. Build and preparation jobs use read-only repository
permissions, pinned action commits, and only `push` (restricted to `main`) and
`workflow_dispatch` triggers. Only the
publication job has `contents: write`, using the built-in GitHub token; no extra secret is
needed for GitHub release publication. Cachix uploads separately require `CACHIX_AUTH_TOKEN`
as described above. The publication job runs exclusively on pushes to `main` or manual runs targeting `main`, after all
test gate, six package jobs and download preparation pass, either in this run or in the verified
identical-tree manual branch run. It never commits to a branch. `actions: read` permits lookup
and download of that run's artifacts.

After a successful run, configure a branch ruleset for `main` requiring these checks:

- `Tests and lint`
- `Nix packages and installed checks`
- `Void glibc package and clean install`
- `Debian 13 package and clean install`
- `Fedora 45 package and clean install`
- `Arch Linux package and clean install`
- `openSUSE Tumbleweed package and clean install`

Package artifacts are retained for 14 days. Neither a passing container check nor a headless
window proves integration with every compositor, GPU, removable drive, or desktop portal
configuration. Those remain real-desktop checks before release. Native ARM runners, Void
musl compatibility, additional distribution recipes, and signed release artifacts
can be added separately without weakening these required checks.

## Native PDF previews

All native recipes package the application with its image, media and PDF helpers.
Only `rummage-photos` is split into an optional package. The PDF helper and private renderer live together in
`/usr/libexec/rummage`; only that helper has an `$ORIGIN` runtime library path.
The main executable does not link PDFium or FFmpeg; the standard package includes
the isolated helpers and their dependencies.

`nix/pdfium-sources.nix` exports source code from the locked Nixpkgs recipes,
including GN and private dependency sources. `export-source.sh` puts this into
the checksum-verified source archive. Nix is needed only on the export host;
Debian source-package rebuilds and native container builds compile everything
with the distro toolchain without fetching additional source or using Nix binaries.

`packaging/pdfium/build.sh` keeps V8, XFA, Skia and PartitionAlloc disabled,
builds ICU common with stub data, and links private dependency archives into
PDFium. System font, colour and image libraries remain shared. Licenses and
source versions are installed with the PDF helper. Updating `flake.lock` also
updates these sources, and requires rerunning the complete native matrix.

Clean installation checks the standard package and its bundled preview helpers,
then installs and removes the photo add-on. It checks runtime libraries
before installing test tools and uses `scripts/ci/check-pdf.py` to exercise the
installed helper's sandbox, first-page rendering, rotation, fonts and forms.
The release assembler requires the standard and photo packages for every target.

### PDF renderer maintenance

`nix build .#pdfium` builds the renderer separately. Update `flake.lock` to update
its source pins and recipe, then rerun the PDF tests and package preflight. Review
`nix/pdfium-read-error.patch` on updates: it propagates a refused custom-file read
instead of terminating the helper on an assertion. Compare rendered output and
timings, and measure the complete added closure against `.#rummage`, not just the
size of `libpdfium.so`.

## Sanitizer and fuzz gate

The `instrumentation` CI matrix runs before the ordinary test/package gate. Its
Clang toolchain, optional preview libraries, fonts and software renderer come
from the locked `instrumentation` Nix development shell. Run one profile locally
with a new output directory outside the checkout:

```sh
nix develop .#instrumentation --command python3 scripts/ci/instrumentation.py asan /tmp/rummage-asan
nix develop .#instrumentation --command python3 scripts/ci/instrumentation.py tsan /tmp/rummage-tsan
nix develop .#instrumentation --command python3 scripts/ci/instrumentation.py fuzz /tmp/rummage-fuzz
```

ASan/UBSan builds and checks the base, FFmpeg and PDFium helper integrations.
The shared libraries supplied by Nix are not themselves rebuilt with sanitizers.
The allocator-accounting process `memory-tests` is excluded because ASan changes
allocation sizes; the normal test gate still runs it. TSan runs the event-loop,
worker-pool and file-watcher suites, with a nonempty-results check. UI/driver,
D-Bus and helper-process tests are outside that TSan scope and remain covered by
the ordinary and ASan jobs. TSan shadow-address mapping failures on incompatible
kernels/containers are failures, never silently converted into passes. No global
ASLR or security-policy changes are made by the runner.

Fuzz smoke tests cover all eight current targets: TIFF, camera, picture, DNG,
lossless JPEG, text, SVG and archives. Each receives a fixed random seed,
15-second mutation budget, 10-second per-input limit, 2 GiB RSS limit and 1 MiB
maximum input size. The outer runner additionally enforces wall-clock deadlines.
Checked-in fixtures are copied into disposable corpora; lossless JPEG starts
with small synthetic inputs, so this is a smoke check, not comprehensive format
coverage. Increase `--fuzz-seconds` up to 60 for a local smoke run. Longer research
runs should invoke the individual fuzz binaries directly.

Logs, identity, command arguments, test reports, mutated corpora and reproducing
inputs are retained as CI artifacts for 14 days, including on failure. Existing
fontconfig leak and Mesa/AMDGPU race suppressions remain narrowly documented in
`src/core/src/lsan_suppressions.cpp`; the runner adds none. Do not disable a failing
sanitizer to obtain a passing package build.

## Preparing a Fedora COPR source package

There is no provisioned project COPR repository. The local preparation workflow
below does not enable a repository, upload packages or claim clean-install
validation. The initial target policy follows the existing native RPM matrix:
Fedora 45 on x86_64. Additional supported Fedora releases and aarch64 require
separate dependency, build, installation and upgrade validation before being
advertised. Use separate stable and development projects: stable accepts an
explicit `vVERSION` tag, while development accepts an identifiable committed
revision. Never automatically promote a development package to stable.

First commit the intended source and use the existing exporter. It vendors the
hash-pinned PDFium sources so the RPM build does not fetch code from the network:

```sh
bash scripts/ci/export-source.sh /tmp/rummage-export
python3 scripts/ci/copr-source.py /tmp/rummage-export/input /tmp/rummage-copr --channel development
rpmbuild -bs --define '_topdir /tmp/rummage-copr' /tmp/rummage-copr/SPECS/rummage.spec
```

For a stable build use `--channel stable --tag vVERSION`, replacing `VERSION`
with the source version. The helper verifies that the existing local tag points
to the exported commit. It verifies the archive hash, uses the recipe inside
that archive, and records both archive and generated-recipe hashes. Development
RPM releases begin with `0.TIMESTAMP.gitCOMMIT`, below the stable `1` release for
the same version. Preserve `source.json` beside the source RPM. Reproducible
source inputs do not guarantee byte-identical binaries as Fedora repositories
continue to update.

Provisioning requires a Fedora account and an agreed project owner with backup
maintainers. The owner must create separate stable/development projects, select
the validated chroots and retain their COPR build IDs and source manifests. A
release automation integration should submit only the prepared source RPM after
local validation, using a scoped credential stored in the CI secret store;
credentials must not appear in source archives or logs. This repository does not
automatically submit to COPR.

Before publishing enablement instructions, verify the actual project URL and
signing-key fingerprint, then test `dnf copr enable OWNER/PROJECT` in a clean
Fedora environment. `OWNER/PROJECT` is a placeholder, not an available repository.
Document who controls that signing key and that enabling a third-party
repository trusts its package updates. Test base-only, `rummage-media`,
`rummage-pdf`, `rummage-font`, `rummage-model` and `rummage-previews` installs,
exact-version dependencies, helper
isolation, upgrade from the previous release, downgrade/channel switching and
removal. Use `scripts/ci/native.sh rpm` for the existing container package check;
a successful local RPM build does not establish COPR service or repository
upgrade behavior.
