#!/usr/bin/env python3
"""Apply the same system-library substitutions as the pinned Nix recipe."""
from pathlib import Path
import shutil
import sys

sources, prefix = map(Path, sys.argv[1:])
root = Path(__file__).resolve().parents[2]


def replace(path, before, after):
    path = Path(path)
    text = path.read_text()
    if before not in text:
        raise ValueError(f'{path}: expected text missing: {before}')
    path.write_text(text.replace(before, after))


replace('.gn', 'script_executable = "//third_party/cpython3/host/bin/python3"',
        'script_executable = "/usr/bin/python3"')
replace('BUILD.gn', 'component("pdfium")', 'shared_library("pdfium")')
replace('public/fpdfview.h', '#if defined(COMPONENT_BUILD)',
        '#if defined(COMPONENT_BUILD) || defined(FPDF_IMPLEMENTATION)')
Path('build/config/gclient_args.gni').write_text('build_with_chromium = false\n')
for name in ('abseil-cpp', 'icu', 'libyuv', 'highway', 'simdutf', 'googletest'):
    target = Path('third_party') / name
    target.mkdir(exist_ok=True)
    recipe = (root / 'nix/pdfium-icu.gn' if name == 'icu' else
              sources / f'system-{name}.gn' if name in ('abseil-cpp', 'googletest') else
              Path('build/linux/unbundle') / f'{name}.gn')
    shutil.copyfile(recipe, target / 'BUILD.gn')
replace('third_party/abseil-cpp/BUILD.gn', '"absl_cleanup"', '"absl_pdfium"')
replace('third_party/abseil-cpp/BUILD.gn', '"absl_flat_hash_set",', '')
replace('third_party/abseil-cpp/BUILD.gn', '"absl_inlined_vector",', '')
for target, source in {
    'third_party/abseil-cpp/absl': prefix / 'include/absl',
    'third_party/fast_float/src/include': sources / 'fast-float/include',
    'third_party/highway/src/hwy.gni': sources / 'highway/hwy.gni',
    'third_party/googletest/src/googletest/include/gtest': prefix / 'include/gtest',
    'third_party/googletest/src/googlemock/include/gmock': prefix / 'include/gmock',
}.items():
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        target.unlink()
    target.symlink_to(source)
target = Path('third_party/googletest/system/gtest/internal/custom')
target.mkdir(parents=True, exist_ok=True)
shutil.copyfile('third_party/googletest/custom/gtest/internal/custom/gtest-printers.h',
                target / 'gtest-printers.h')
