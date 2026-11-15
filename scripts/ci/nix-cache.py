#!/usr/bin/env python3
"""Transfer the exact installable packages and successful checks across a merge."""
import argparse
import json
from pathlib import Path
import subprocess

from preflight import NIX_PACKAGES

ATTRIBUTES = [*NIX_PACKAGES, *(
    f'.#checks.x86_64-linux.{name}' for name in ('tests', 'smoke', 'nixos-module'))]


def roots():
    paths = []
    for attribute in ATTRIBUTES:
        paths.extend(json.loads(subprocess.check_output([
            'nix', 'eval', '--json', attribute,
            '--apply', 'p: map (name: p.${name}.outPath) p.outputs',
        ], text=True)))
    return sorted(set(paths))


def transfer(mode, directory, run=subprocess.run):
    paths = roots()
    if mode == 'push':
        run(['cachix', 'push', 'rummage-fm', *paths], check=True)
        return
    uri = directory.resolve().as_uri()
    if mode == 'export':
        directory.mkdir(parents=True, exist_ok=True)
        run(['nix', 'copy', '--to', uri, *paths], check=True)
    else:
        # Called only on main after matching the PR's entire Git tree and successful jobs.
        # Resolve roots from this checkout, never from an artifact-provided path list.
        run(['nix', 'copy', '--from', uri, '--no-check-sigs', *paths], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('export', 'restore', 'push'))
    parser.add_argument('directory', nargs='?', type=Path)
    args = parser.parse_args()
    if args.mode != 'push' and args.directory is None:
        parser.error('export/restore requires a binary-cache directory')
    transfer(args.mode, args.directory)
