import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "i18n_check", Path(__file__).resolve().parents[2] / "i18n-check.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class TranslationChecks(unittest.TestCase):
    def test_malformed_tables_report_errors(self):
        for catalog in ({"language": "broken"}, {"texts": "broken"},
                        {"contexts": "broken"}, {"contexts": {"action": "broken"}}):
            self.assertTrue(check.validate_catalog(catalog, {}, set())[2])

    def test_context_and_plural_calls(self):
        used = check.calls('''
          i18n::tr("Plain"); i18n::trf("{} done", n);
          i18n::trc("action", "Open"); i18n::trcf("status", "{} done", n);
          i18n::trn("{} file", "{} files", n);
          i18n::trnc("status", "{} file " "ready", "{} files ready", n);
        ''')
        self.assertEqual(used[("status", "{} files ready")], "{} file ready")
        self.assertIn(("action", "Open"), used)
        self.assertEqual(len(used), 6)

    def test_rules_reject_malformed_and_unbounded_input(self):
        for rule in ("n = 0..1", "n % 10 = 2..4 and n % 100 != 12..14", "n = 0 or n = 1"):
            self.assertTrue(check.valid_rule(rule), rule)
        for rule in ("n % 0 = 1", "n = -1", "n = 4..2", "n = 1 or", "n = 1.2",
                     "n = 18446744073709551616", "n = 1 extra", " " * 1025):
            self.assertFalse(check.valid_rule(rule), rule)

    def test_plural_forms_and_placeholders_are_checked(self):
        catalog = {"language": {"name": "Fixture", "plural_rules": {"few": "n = 2..4"}},
                   "texts": {"{} files": {"few": "{} files-few", "other": "{} files-other"}},
                   "contexts": {"action": {"Open": "Open-action"}}}
        used = {("", "{} files"): "{} file", ("action", "Open"): None}
        self.assertEqual(check.validate_catalog(catalog, used, set()), ([], [], []))
        del catalog["texts"]["{} files"]["few"]
        self.assertTrue(check.validate_catalog(catalog, used, set())[2])
        catalog["texts"]["{} files"]["few"] = "{1} wrong field"
        self.assertTrue(check.validate_catalog(catalog, used, set())[2])

    def test_contexts_do_not_hide_missing_entries(self):
        catalog = {"language": {"name": "Fixture"}, "texts": {"Open": "Generic"},
                   "contexts": {"state": {"Open": "Open-state"}}}
        missing, unused, _ = check.validate_catalog(catalog, {("action", "Open"): None}, {"Open"})
        self.assertEqual(missing, [("action", "Open")])
        self.assertEqual(unused, [("state", "Open")])

    def test_format_fields_can_reorder_but_not_disappear(self):
        self.assertEqual(check.fields("{} in {}"), check.fields("{1}: {0}"))
        self.assertEqual(check.fields("{{x}} {}"), check.fields("{} {{x}}"))
        self.assertEqual(check.fields("{name} {ext}"), check.fields("{ext} {name}"))
        self.assertNotEqual(check.fields("{} {}"), check.fields("{}"))
        with self.assertRaises(ValueError):
            check.fields("{} {1}")
