#!/usr/bin/env python3
"""Build a benchmark-only executable against an existing Release build.

The original source and executable are never changed. Both projects receive
identical probes; their helpers and assets are reused through symlinks. Run inside
the same development environment used to build the original application.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--prefix', choices=('rummage', 'nordend'), required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.source, a.build, a.out = a.source.resolve(), a.build.resolve(), a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    source = a.source / 'src/app/main.cpp'
    original = source.read_text()
    code = '#include ' + json.dumps(str(Path(__file__).with_name('probe.hpp').resolve())) + '\n' + original
    patches = [
        ('seat.set_key_handler([&](const ui::KeyEvent& event) {',
         'seat.set_key_handler([&](const ui::KeyEvent& event) {\n            performance_probe::key(event.keysym);'),
        ('window.set_frame_shown_handler([&] {',
         'window.set_frame_shown_handler([&] {\n                performance_probe::frame();'),
        ('note_state();  // every change shows in a frame, so frames are where to look',
         'performance_probe::draw(*columns, window.width(), window.height());\n                note_state();  // every change shows in a frame, so frames are where to look'),
    ]
    for before, after in patches:
        if code.count(before) != 1:
            raise RuntimeError(f'Instrumentation anchor changed: {before}')
        code = code.replace(before, after)
    temp_source = a.out / 'main.cpp'
    temp_source.write_text(code)
    commands = subprocess.check_output(['ninja', '-C', str(a.build), '-t', 'commands', a.prefix], text=True).splitlines()
    compile_line = next(c for c in commands if ' -c ' in c and c.endswith('/src/app/main.cpp'))
    compile_args = shlex.split(compile_line)
    old_object = compile_args[compile_args.index('-o') + 1]
    obj = str(a.out / 'main.o')
    compile_args[compile_args.index('-o') + 1] = obj
    compile_args[compile_args.index('-c') + 1] = str(temp_source)
    if '-MF' in compile_args:
        compile_args[compile_args.index('-MF') + 1] = obj + '.d'
    # Preserve quoted definitions, linker grouping and CMake's harmless ':' wrappers.
    link_line = commands[-1]
    link_line = link_line.replace(old_object, shlex.quote(obj))
    link_line = link_line.replace(' -o ' + a.prefix + ' ', ' -o ' + shlex.quote(str(a.out / a.prefix)) + ' ')
    subprocess.run(compile_args, cwd=a.build, check=True)
    subprocess.run(link_line, cwd=a.build, shell=True, check=True)
    for child in a.build.iterdir():
        dest = a.out / child.name
        if child.name.startswith(a.prefix + '-') or child.name == 'engines':
            if not dest.exists():
                dest.symlink_to(child.resolve())
    (a.out / 'instrumentation.json').write_text(json.dumps({
        'source': str(source), 'source_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'probe_sha256': hashlib.sha256(Path(__file__).with_name('probe.hpp').read_bytes()).hexdigest(),
        'compile': compile_args, 'link': link_line,
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
