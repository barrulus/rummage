import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('noctalia_template', SCRIPTS / 'noctalia-template.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

ROLES = check.palette_roles(check.PALETTE_HEADER.read_text())
CATALOG = '''[catalog.rummage]
name = "rummage"
category = "system"

[templates.rummage]
input_path = "colors.toml"
output_path = "$XDG_CONFIG_HOME/rummage/colors.toml"
'''


def explicit(roles, source=lambda role: role):
    lines = ['# comment', 'mode = "{{ mode }}"', '', '[colors]']
    lines += [f'{role} = "{{{{ colors.{source(role)}.default.hex }}}}"' for role in roles]
    return '\n'.join(lines) + '\n'


class NoctaliaTemplateChecks(unittest.TestCase):
    def run_check(self, colors, catalog=CATALOG):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / 'rummage'
            directory.mkdir()
            if colors is not None:
                (directory / 'colors.toml').write_text(colors)
            if catalog is not None:
                (directory / 'template.toml').write_text(catalog)
            report = Path(tmp) / 'report.md'
            with patch('builtins.print'):
                status = check.main(['--dir', tmp, '--report', str(report)])
            return status, report.read_text() if report.exists() else ''

    def test_roles_come_from_the_palette_struct(self):
        self.assertIn('primary', ROLES)
        self.assertIn('on_hover', ROLES)
        self.assertNotIn('dark', ROLES)
        self.assertGreater(len(ROLES), 30)

    def test_own_template_covers_every_noctalia_colour(self):
        own = check.parse_template(check.OWN_TEMPLATE.read_text())
        self.assertTrue(own['mode'])
        self.assertTrue(own['all'])
        self.assertEqual(own['unknown'], [])

    def test_matching_template_passes(self):
        status, report = self.run_check(explicit(ROLES))
        self.assertEqual(status, 0)
        self.assertIn('matches', report)

    def test_invalid_color_toml_is_drift(self):
        colors = explicit(ROLES)
        cases = {
            'missing closing bracket': colors.replace('[colors]', '[colors'),
            'array of tables': colors.replace('[colors]', '[[colors]]'),
            'duplicate color': colors + 'primary = "{{ colors.primary.default.hex }}"\n',
            'duplicate mode': colors.replace('mode =', 'mode = "{{ mode }}"\nmode ='),
            'duplicate table': colors + '[colors]\n',
        }
        for name, invalid in cases.items():
            with self.subTest(name=name):
                status, report = self.run_check(invalid)
                self.assertEqual(status, 1)
                self.assertIn('colors.toml:', report)
                self.assertNotIn('matches rummage', report)

    def test_missing_extra_and_remapped_roles_are_reported(self):
        roles = [role for role in ROLES if role != 'on_hover'] + ['accent']
        colors = explicit(roles, lambda role: 'primary' if role == 'hover' else role)
        status, report = self.run_check(colors)
        self.assertEqual(status, 1)
        self.assertIn('`on_hover` is missing', report)
        self.assertIn('`accent` is not a rummage palette role', report)
        self.assertIn('`hover` is filled from `colors.primary`', report)

    def test_mode_output_path_and_unknown_lines_are_reported(self):
        colors = explicit(ROLES).replace('mode = "{{ mode }}"\n', '') + 'odd line\n'
        catalog = CATALOG.replace('$XDG_CONFIG_HOME', '~/.config')
        status, report = self.run_check(colors, catalog)
        self.assertEqual(status, 1)
        self.assertIn('no `mode', report)
        self.assertIn('output_path is `~/.config/rummage/colors.toml`', report)
        self.assertIn('not understood: `odd line`', report)

    def test_unpublished_template_is_not_drift(self):
        status, report = self.run_check(None, None)
        self.assertEqual(status, 3)
        self.assertEqual(report, '')

    def test_only_one_missing_file_is_drift(self):
        for colors, catalog in ((None, CATALOG), (explicit(ROLES), None)):
            with self.subTest(colors_missing=colors is None):
                status, report = self.run_check(colors, catalog)
                self.assertEqual(status, 1)
                self.assertIn('differs from rummage', report)


class NoctaliaIssueChecks(unittest.TestCase):
    def run_issue_step(self, status):
        # Exercise the workflow's actual shell with an existing drift issue;
        # the stub records all gh calls and never contacts GitHub.
        workflow = (check.ROOT / '.github/workflows/noctalia-template.yml').read_text()
        step = workflow.split('      - name: Report in an issue\n', 1)[1]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1].split('\n      - ', 1)[0])
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            gh = directory / 'gh'
            gh.write_text('#!/bin/sh\n'
                          'printf "%s\\n" "$*" >> "$NOCTALIA_TEST_CALLS"\n'
                          'if [ "$2" = list ]; then echo 42; fi\n'
                          'if [ "$2" = view ]; then echo "Previous drift"; fi\n')
            gh.chmod(0o755)
            calls = directory / 'calls'
            (directory / 'report.md').write_text('Current drift\n')
            env = {**os.environ, 'PATH': str(directory) + os.pathsep + os.environ['PATH'],
                   'STATUS': str(status), 'RUNNER_TEMP': tmp,
                   'GITHUB_REPOSITORY': 'test/rummage', 'RUN_URL': 'https://example.com/run',
                   'NOCTALIA_TEST_CALLS': str(calls)}
            subprocess.run(['bash', '-euo', 'pipefail', '-c', script], env=env,
                           check=True, capture_output=True, text=True)
            return calls.read_text().splitlines()

    def test_only_a_successful_comparison_closes_an_existing_issue(self):
        calls = self.run_issue_step(0)
        self.assertTrue(any(call.startswith('issue close 42 ') for call in calls))

    def test_skipped_comparison_and_fetch_error_leave_existing_issue_untouched(self):
        for status in (2, 3):
            with self.subTest(status=status):
                calls = self.run_issue_step(status)
                self.assertTrue(all(call.startswith('issue list ') for call in calls))

    def test_drift_updates_the_existing_issue(self):
        calls = self.run_issue_step(1)
        self.assertTrue(any(call.startswith('issue comment 42 ') for call in calls))
        self.assertTrue(any(call.startswith('issue edit 42 ') for call in calls))
        self.assertFalse(any(call.startswith('issue close ') for call in calls))


if __name__ == '__main__':
    unittest.main()
