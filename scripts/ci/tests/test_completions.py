"""Keep completions aligned with the public CLI and exercise shell argument parsing."""
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
             'type compgen complete >/dev/null 2>&1 || { '
             'echo "Completion tests need Bash with programmable completion (nixpkgs#bashInteractive)." >&2; '
             'exit 1; }; '
             'source "$1" || exit; shift; COMP_WORDS=("$@"); '
             'COMP_CWORD=$((${#COMP_WORDS[@]} - 1)); _rummage; '
             'if ((${#COMPREPLY[@]})); then printf "%s\\n" "${COMPREPLY[@]}"; fi',
             "completion-test", str(COMPLETIONS / "rummage.bash"), "rummage", *words],
            cwd=directory, check=False, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
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

    def test_bash_launch_modes_are_exclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode in ("--pick", "--pick-directory", "--save-file", "--reveal", "--portal"):
                options = self.complete(directory, mode, "--")
                self.assertNotIn("--pick", options)
                self.assertNotIn("--portal", options)
                self.assertNotIn("--new-window", options)
            self.assertIn("--new-window", self.complete(directory, "--open-folders", "--"))
            self.assertIn("--pick", self.complete(directory, "--config", "--portal", "--"))

    def zsh_arguments(self, *words):
        if not shutil.which("zsh"):
            self.skipTest("zsh unavailable")
        result = subprocess.run(
            ["zsh", "-f", "-c",
             'completion=$1; shift; words=("$@"); CURRENT=${#words}; '
             'function _arguments() { printf "%s\\n" "$@"; }; source "$completion"',
             "completion-test", str(COMPLETIONS / "_rummage"), "rummage", *words],
            text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.splitlines()

    def test_zsh_launch_modes_and_flag_like_values(self):
        self.assertTrue(any(")--pick[" in x for x in self.zsh_arguments("--")))
        picked = self.zsh_arguments("--pick", "--")
        mode_spec = next(x for x in picked if ")--pick[" in x)
        self.assertIn("--portal", mode_spec.split(")", 1)[0])
        self.assertIn("--pick-directory", mode_spec.split(")", 1)[0])
        self.assertIn("--multiple[Choose multiple files]", picked)
        values = self.zsh_arguments("--config", "--pick", "--")
        self.assertNotIn("--multiple[Choose multiple files]", values)
        self.assertIn("*:folder:_files -/", self.zsh_arguments("--", "--reveal", ""))
        self.assertIn("*:path:_files", self.zsh_arguments("--reveal", "folder with spaces"))

    def fish_complete(self, directory, command):
        if not shutil.which("fish"):
            self.skipTest("fish unavailable")
        result = subprocess.run(
            ["fish", "--no-config", "-c", 'source $argv[1]; complete -C $argv[2]',
             str(COMPLETIONS / "rummage.fish"), command],
            cwd=directory, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return {line.split("\t")[0] for line in result.stdout.splitlines()}

    def test_fish_context_and_escaped_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "folder with spaces").mkdir()
            (root / "file.txt").touch()
            self.assertNotIn("--pick", self.fish_complete(root, "rummage --pick --"))
            self.assertIn("--multiple", self.fish_complete(root, "rummage --pick --"))
            self.assertNotIn("--multiple", self.fish_complete(root, "rummage --config --pick --"))
            self.assertIn("file.txt", self.fish_complete(root, "rummage --reveal file"))
            self.assertNotIn("file.txt", self.fish_complete(root, "rummage file"))
            paths = self.fish_complete(root, "rummage folder")
            self.assertTrue(any("folder" in p and "spaces" in p for p in paths), paths)


if __name__ == "__main__":
    unittest.main()
