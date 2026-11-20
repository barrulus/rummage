#!/usr/bin/env python3
"""Compare Noctalia's community template for rummage with rummage's own palette interface.

The community template (noctalia-dev/community-templates, directory rummage/) is maintained
outside this repository. It drifts when rummage gains or renames a palette role, or when the
template changes upstream. This check reads the roles from the Palette struct, the mapping from
rummage's own Noctalia template, and reports every difference. Exit status: 0 in sync,
1 drift, 2 the template could not be fetched, 3 not published (comparison skipped).
"""
import argparse
from pathlib import Path
import re
import sys
import tomllib
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PALETTE_HEADER = ROOT / 'src/theme/include/rummage/theme/palette.hpp'
OWN_TEMPLATE = ROOT / 'data/noctalia/rummage'
OUTPUT_PATH = '$XDG_CONFIG_HOME/rummage/colors.toml'
TEMPLATE_DIR = 'rummage'

ROLE_LINE = re.compile(r'^\s*Color\s+(\w+)\s*\{')
MODE_LINE = re.compile(r'^mode\s*=\s*"\{\{\s*mode\s*\}\}"\s*$')
COLOR_LINE = re.compile(r'^(\w+)\s*=\s*"\{\{\s*colors\.(\w+)\.default\.hex\s*\}\}"\s*$')
LOOP_START = re.compile(r'^<\*\s*for\s+(\w+)\s*,\s*(\w+)\s+in\s+colors\s*\*>$')
LOOP_END = re.compile(r'^<\*\s*endfor\s*\*>$')


def palette_roles(header):
    """Colour roles of struct Palette, in declaration order."""
    match = re.search(r'^struct Palette \{\n(.*?)^\};', header, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit('::error::struct Palette not found in palette.hpp')
    body = match.group(1)
    return [m.group(1) for line in body.splitlines() if (m := ROLE_LINE.match(line))]


def parse_template(text):
    """Return mode presence, whether all colours are looped through, the explicit
    key -> source role mapping, and lines that are not understood."""
    result = {'mode': False, 'all': False, 'colors': {}, 'unknown': []}
    section = ''
    loop = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if loop:
            if LOOP_END.match(line):
                loop = None
            elif not re.fullmatch(r'\{\{\s*%s\s*\}\}\s*=\s*"\{\{\s*%s\.default\.hex\s*\}\}"' % loop,
                                  line):
                result['unknown'].append((number, line))
            continue
        if line.startswith('['):
            section = line.strip('[] ')
        elif section == '' and MODE_LINE.match(line):
            result['mode'] = True
        elif section == 'colors' and (m := COLOR_LINE.match(line)):
            result['colors'][m.group(1)] = m.group(2)
        elif section == 'colors' and (m := LOOP_START.match(line)):
            loop = m.groups()
            result['all'] = True
        else:
            result['unknown'].append((number, line))
    return result


def expected_mapping(roles, own):
    """key -> source role as rummage's own template writes it."""
    if own['all']:
        return {role: role for role in roles}
    return dict(own['colors'])


def validate_colors(text):
    """The explicit community template must be TOML before placeholder expansion.

    Keep this separate from parse_template: our own template uses Noctalia loop
    directives, which are not TOML until rendered.
    """
    try:
        document = tomllib.loads(text)
    except tomllib.TOMLDecodeError as error:
        return [f'colors.toml: not valid TOML: {error}']
    if not isinstance(document.get('colors'), dict):
        return ['colors.toml: expected a `[colors]` table.']
    return []


def compare(roles, own, community, catalog):
    problems = []
    if not community['mode']:
        problems.append('colors.toml: no `mode = "{{ mode }}"` line; rummage cannot tell dark from light.')
    if community['all']:
        problems.append('colors.toml: loops over all colours; review the comparison by hand.')
    expected = expected_mapping(roles, own)
    written = community['colors']
    for role in roles:
        if role not in written:
            problems.append(f'colors.toml: `{role}` is missing; rummage reads it and would use '
                            'the desktop baseline instead.')
        elif role in expected and written[role] != expected[role]:
            problems.append(f'colors.toml: `{role}` is filled from `colors.{written[role]}`; '
                            f'rummage\'s template uses `colors.{expected[role]}`.')
    for key in written:
        if key not in roles:
            problems.append(f'colors.toml: `{key}` is not a rummage palette role and is ignored.')
    for number, line in community['unknown']:
        problems.append(f'colors.toml:{number}: not understood: `{line}`')

    templates = catalog.get('templates', {})
    entry = templates.get(TEMPLATE_DIR)
    if not isinstance(entry, dict):
        problems.append(f'template.toml: no `[templates.{TEMPLATE_DIR}]` entry.')
    else:
        if entry.get('output_path') != OUTPUT_PATH:
            problems.append(f'template.toml: output_path is `{entry.get("output_path")}`; '
                            f'rummage reads `{OUTPUT_PATH}`.')
        if entry.get('input_path') != 'colors.toml':
            problems.append(f'template.toml: input_path is `{entry.get("input_path")}`; '
                            'this check compares `colors.toml`.')
    return problems


def fetch(repo, ref, name):
    url = f'https://raw.githubusercontent.com/{repo}/{ref}/{TEMPLATE_DIR}/{name}'
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            return response.read().decode()
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def report(problems, source):
    if problems:
        lines = [f'Noctalia\'s community template for rummage ({source}) differs from rummage:', '']
        lines += [f'- {problem}' for problem in problems]
        lines += ['', 'Update the template upstream (noctalia-dev/community-templates) or rummage\'s '
                  'palette roles and `data/noctalia/rummage`, whichever is out of date.']
    else:
        lines = [f'Noctalia\'s community template for rummage ({source}) matches rummage\'s palette roles.']
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', default='noctalia-dev/community-templates')
    parser.add_argument('--ref', default='main')
    parser.add_argument('--dir', type=Path, help='local community-templates checkout instead of GitHub')
    parser.add_argument('--report', type=Path, help='also write the Markdown report here')
    args = parser.parse_args(argv)

    if args.dir:
        source = str(args.dir)
        files = [(args.dir / TEMPLATE_DIR / name) for name in ('colors.toml', 'template.toml')]
        texts = [path.read_text() if path.is_file() else None for path in files]
    else:
        source = f'{args.repo}@{args.ref}'
        try:
            texts = [fetch(args.repo, args.ref, name) for name in ('colors.toml', 'template.toml')]
        except (urllib.error.URLError, TimeoutError) as error:
            print(f'::error::cannot fetch the community template from {source}: {error}')
            return 2
    if texts[0] is None and texts[1] is None:
        print(f'::notice::{source} has no {TEMPLATE_DIR}/ template yet; nothing to compare.')
        return 3

    roles = palette_roles(PALETTE_HEADER.read_text())
    own = parse_template(OWN_TEMPLATE.read_text())
    community = parse_template(texts[0] or '')
    problems = validate_colors(texts[0]) if texts[0] is not None else []
    try:
        catalog = tomllib.loads(texts[1] or '')
    except tomllib.TOMLDecodeError as error:
        catalog = {}
        problems.append(f'template.toml: not valid TOML: {error}')
    if texts[0] is None:
        problems.append('colors.toml is missing.')
    problems += compare(roles, own, community, catalog) if texts[0] is not None else []

    text = report(problems, source)
    print(text, end='')
    if args.report:
        args.report.write_text(text)
    for problem in problems:
        print(f'::error title=Noctalia template drift::{problem}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
