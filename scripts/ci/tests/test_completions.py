"""Keep completions aligned with the public CLI and exercise bash argument parsing."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
COMPLETIONS = ROOT / "data/completions"


class CompletionsTest(unittest.TestCase):
    def test_public_options_match_all_shells(self):
        source = (ROOT / "src/app/main.cpp").read_text()
        usage = source[source.index('"Picker:'):source.index('// --about:')]
        expected = set(re.findall(r"--[a-z][a-z-]+", usage)) | {"--help"}
        for name in ("rummage.bash", "_rummage", "rummage.fish"):
            text = (COMPLETIONS / name).read_text()
            actual = ({"--" + x for x in re.findall(r"^complete .*? -l ([a-z-]+)", text, re.M)}
                      if name.endswith(".fish") else set(re.findall(r"--[a-z][a-z-]+", text)))
            self.assertEqual(actual, expected, name)

    def complete(self, directory, *words):
        if not shutil.which("bash"):
            self.skipTest("bash unavailable")
        result = subprocess.run(
            ["bash", "--noprofile", "--norc", "-c",
             'source "$1"; shift; COMP_WORDS=("$@"); '
             'COMP_CWORD=$((${#COMP_WORDS[@]} - 1)); _rummage; '
             'if ((${#COMPREPLY[@]})); then printf "%s\\n" "${COMPREPLY[@]}"; fi',
             "completion-test", str(COMPLETIONS / "rummage.bash"), "rummage", *words],
            cwd=directory, check=True, text=True, capture_output=True)
        return set(result.stdout.splitlines())

    def test_context_paths_and_option_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "folder with spaces").mkdir()
            (root / "file.txt").touch()
            (root / "--config-file").mkdir()
            self.assertIn("folder with spaces", self.complete(root, "folder"))
            self.assertNotIn("file.txt", self.complete(root, "file"))
            self.assertIn("file.txt", self.complete(root, "--reveal", "file"))
            self.assertIn("file.txt", self.complete(root, "--config", "file"))
            self.assertEqual(self.complete(root, "--save-file", "--title", "--"), set())
            self.assertIn("--multiple", self.complete(root, "--pick-directory", "--"))
            self.assertNotIn("--multiple", self.complete(root, "--"))
            self.assertNotIn("--multiple", self.complete(root, "--config", "--pick", "--"))
            self.assertEqual(self.complete(root, "--", "--config"), {"--config-file"})
            self.assertEqual(self.complete(root, "--", "--reveal", "file"), set())


if __name__ == "__main__":
    unittest.main()
