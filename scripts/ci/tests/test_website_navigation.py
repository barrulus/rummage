import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "website_check", Path(__file__).resolve().parents[3] / "website" / "check.py")
website_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(website_check)


class WebsiteNavigationTest(unittest.TestCase):
    def test_omitted_guide_fails_even_without_an_incoming_site_link(self):
        with tempfile.TemporaryDirectory() as directory:
            docs = Path(directory)
            for name in ("README.md", "usage.md", "terminal.md"):
                (docs / name).write_text("# Guide\n")
            navigation = {"groups": [{"items": [{"slug": "usage"}]}]}
            self.assertEqual(website_check.navigation_errors(navigation, docs), [
                "Canonical guide missing from website navigation: terminal"])
            navigation["groups"][0]["items"].append({"slug": "terminal"})
            self.assertEqual(website_check.navigation_errors(navigation, docs), [])

    def test_duplicate_and_stale_navigation_entries_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            docs = Path(directory)
            (docs / "usage.md").write_text("# Guide\n")
            navigation = {"groups": [{"items": [
                {"slug": "usage"}, {"slug": "usage"}, {"slug": "removed"}]}]}
            self.assertEqual(website_check.navigation_errors(navigation, docs), [
                "Website navigation has no canonical guide: removed",
                "Duplicate website navigation guide: usage"])
