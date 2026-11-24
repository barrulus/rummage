#!/usr/bin/env python3
"""Promote identical-tree manually tested branch packages while retaining their original source identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def promote(directory, tested_commit, commit, tree, tested_tree):
    if any(not re.fullmatch(r'[0-9a-f]{40}', value) for value in (tested_commit, commit, tree, tested_tree)):
        raise ValueError('Expected complete commit and tree hashes')
    if tree != tested_tree:
        raise ValueError('Tested source tree differs from the merged tree')
    manifest = directory / 'SHA256SUMS'
    lines = manifest.read_text().splitlines()
    assets = []
    for line in lines:
        digest, name = line.split('  ')
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name) or name in assets:
            raise ValueError('Invalid checksum filename')
        path = directory / name
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('Asset checksum mismatch')
        assets.append(name)
    if 'source.json' not in assets:
        raise ValueError('Source identity is not checksummed')
    identity_file = directory / 'source.json'
    identity = json.loads(identity_file.read_text())
    if identity['commit'] != tested_commit:
        raise ValueError('Artifacts differ from the selected tested commit')
    # Preserve commit, timestamp, source archive and package versions exactly as built.
    identity.update(published_commit=commit, tree=tree)
    identity_file.write_text(json.dumps(identity, indent=2) + '\n')
    manifest.write_text(''.join(
        f'{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n' for name in assets))
    notes = directory / 'release-notes.md'
    notes.write_text(
        f'Promoted without rebuilding to main commit `{commit}`. '
        f'The complete source tree matches the tested branch checkout (`{tree}`). '
        'Package versions and source archives retain the original tested commit.\n\n' + notes.read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--tested-commit', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--repository', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9a-f]{40}', args.tested_commit):
        parser.error('Expected a full tested commit SHA')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', args.repository):
        parser.error('Invalid repository')
    tree = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], text=True).strip()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    if head != args.commit:
        parser.error('Checkout differs from the publication target')
    tested = json.loads(subprocess.check_output([
        'gh', 'api', f'repos/{args.repository}/git/commits/{args.tested_commit}',
    ], text=True))
    promote(args.directory, args.tested_commit, args.commit, tree, tested['tree']['sha'])
