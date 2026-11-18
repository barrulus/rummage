import importlib.util
from pathlib import Path
import tempfile
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
        self.assertEqual(status, 0)
        self.assertEqual(report, '')


if __name__ == '__main__':
    unittest.main()
