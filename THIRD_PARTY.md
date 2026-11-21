# Third-party material

[Documentation](docs/README.md) · [License](LICENSE)

Code or data taken from other projects, with origin and license. Libraries linked as system
packages are not listed here (see [build dependencies](docs/install.md#build-dependencies)).

| What | Where in rummage | Origin | License |
|------|------------------|--------|---------|
| wuffs, the single-file C library (`release/c/wuffs-v0.4.c`, version 0.4.0-alpha.12+4048.20261020), unmodified; rummage compiles only its PNG, JPEG and GIF decoders and the `wuffs_aux` image API | `third_party/wuffs/` (with `LICENSE-MIT` and `LICENSE-APACHE`) | [google/wuffs](https://github.com/google/wuffs), tag `v0.4.0-alpha.12` (commit `7f70dda`), sha256 of the file `458066457347bd6d9bc3c906aa8292bea4488577727569ddd9436fdd89d5f04d` | MIT or Apache-2.0 |
| Tabler Icons webfont 3.50.0: the outline font `tabler-icons.ttf` and the filled `tabler-icons-filled.ttf`, cut down with fonttools' `pyftsubset` (glyph names kept, no layout features) to the 49 and 3 icons listed in `icons.txt` and `icons-filled.txt`; glyphs otherwise unmodified. Made by `scripts/make-icon-font.sh`; compiled into the program by `cmake/embed.cmake`. sha256 of the subsets: `d86a993cec05d636a99c7192e0972be54f9ee29a90967298605cea9f7e663d3e` (outline), `aabd3aec21dd2d17a627e62a01bc05ed5a032efe04d7a50035a26c4288d59d39` (filled) | `third_party/tabler-icons/` (with the upstream `LICENSE`) | [tabler/tabler-icons](https://github.com/tabler/tabler-icons), npm package [`@tabler/icons-webfont`](https://www.npmjs.com/package/@tabler/icons-webfont) 3.50.0 (tag `v3.50.0`), sha256 of the tarball `1afa9b3764607de419b4eeff296f2328361a5ba605ebd35812832f61ddbf070e` | MIT |
| fzy 1.1, its matcher only: `src/match.c`, `src/match.h`, `src/bonus.h` unmodified, and `config.h` = upstream's `src/config.def.h` unmodified (the copy its Makefile makes); compiled as C on its own, without our warnings, for the command palette's fuzzy matching. sha256: `59e2164938037fe9471235d5537a8ae2360f6ba406a3198f1e69549f3bf43e56` (match.c), `cab6cb2cb02db69967448def71cd70a762cf06de05b3ce4bb9d2e21a006b1787` (match.h), `e6f1e5e6c7742bd4fdd8a8cffa5647b59f3e85960d10ee01b250493aec273abc` (bonus.h), `7823ace3f3b41ab552f1ee4e752782bf9f13ce1d1b018ec56b57d8d2ca1d5385` (config.h) | `third_party/fzy/` (with the upstream `LICENSE`) | [jhawthorn/fzy](https://github.com/jhawthorn/fzy), tag `1.1` (commit `d811eaf`), sha256 of the tarball `93d300d9c6c7063b2c6bda4e08a9704a029ec33f609718cd95443d1a890aff4e`; Noctalia vendors the same release | MIT |
| Default palette: the 33 color values of Noctalia's built-in "Noctalia" palette, dark mode, as Noctalia expands it | `src/theme/include/rummage/theme/palette.hpp` (`Palette` defaults) | [noctalia-dev/noctalia](https://github.com/noctalia-dev/noctalia) v5.3.0, `src/theme/builtin_palettes.cpp` expanded by `src/theme/fixed_palette.cpp` | MIT |

Native source archives additionally contain `third_party/pdfium-sources`, assembled
from the hash-pinned recipes in `flake.lock` by `nix/pdfium-sources.nix`. It includes
PDFium and Chromium build files (BSD), GN (BSD), Abseil (Apache-2.0), ICU (Unicode),
libyuv and OpenJPEG (BSD), Highway (Apache-2.0), simdutf (MIT or Apache-2.0),
fast_float (MIT, Apache-2.0 or BSL-1.0), and GoogleTest (BSD). The source bundle
retains their upstream notices and records versions in `versions.json`.

The optional native `rummage-pdf` package installs the renderer and embedded
components' notices under `/usr/share/licenses/rummage-pdf`, including PDFium's
nested third-party notices. Build tools and test dependencies are not installed
in the runtime package. The source build and private library layout are described
in [native PDF packaging](contributing/ci-packaging.md#native-pdf-previews).
