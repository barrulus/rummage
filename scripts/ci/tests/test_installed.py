"""Exercise installed manifest checks with explicit optional-terminal configurations."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "check-installed.sh"


class InstalledManifestTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.prefix = self.root / "installed prefix"
        files = [
            "share/licenses/rummage/LICENSE", "share/rummage/config.example.toml",
            "share/rummage/engines/base.toml", "share/rummage/engines/media.toml",
            "share/rummage/engines/pdf.toml", "share/applications/rummage.desktop",
            "share/bash-completion/completions/rummage", "share/zsh/site-functions/_rummage",
            "share/fish/vendor_completions.d/rummage.fish",
            "share/icons/hicolor/scalable/apps/rummage.svg", "share/rummage/noctalia/rummage",
            "share/rummage/matugen/rummage.toml",
        ]
        for name in files:
            self.write(name, "fixture\n")
        self.write("share/xdg-desktop-portal/portals/rummage.portal",
                   "Interfaces=org.freedesktop.impl.portal.FileChooser;\n")
        self.write("share/dbus-1/services/org.freedesktop.impl.portal.desktop.rummage.service",
                   f"Exec={self.prefix}/bin/rummage --portal\n")
        self.write("bin/rummage", f"#!{shutil.which('bash')}\n"
                   'case "$1" in\n --version) echo "rummage fixture";;\n'
                   ' --help) echo "--pick";;\n --config) exit 0;;\n *) exit 1;;\nesac\n', True)
        for helper in ("preview", "media", "pdf"):
            self.write(f"libexec/rummage/rummage-{helper}", "fixture\n", True)
        self.helper = self.prefix / "libexec/rummage/rummage-terminal"
        tools = self.root / "tools"
        tools.mkdir()
        validator = tools / "desktop-file-validate"
        validator.write_text(f"#!{shutil.which('bash')}\nexit 0\n")
        validator.chmod(0o755)
        # The launcher fixture is a script; model the ELF dependency report separately.
        readelf = tools / "readelf"
        readelf.write_text(f"#!{shutil.which('bash')}\n"
                           'echo "${RUMMAGE_TEST_NEEDED:-}"\n')
        readelf.chmod(0o755)
        self.env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"]}

    def write(self, name, text, executable=False):
        path = self.prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if executable:
            path.chmod(0o755)

    def run_check(self, mode=None):
        args = ["bash", str(SCRIPT), str(self.prefix), "fixture"]
        if mode is not None:
            args += ["", mode]
        return subprocess.run(args, env=self.env, capture_output=True, text=True)

    def test_enabled_requires_executable_helper_including_default(self):
        for mode in (None, "on"):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.run_check(mode).returncode, 0)
                self.write("libexec/rummage/rummage-terminal", "fixture\n", True)
                self.assertEqual(self.run_check(mode).returncode, 0)
                self.helper.unlink()

    def test_disabled_requires_helper_to_be_absent(self):
        self.assertEqual(self.run_check("off").returncode, 0)
        self.write("libexec/rummage/rummage-terminal", "fixture\n", True)
        self.assertNotEqual(self.run_check("off").returncode, 0)

    def test_unknown_feature_mode_is_not_silently_accepted(self):
        result = self.run_check("unexpected")
        self.assertEqual(result.returncode, 2)
        self.assertIn("terminal mode must be on or off", result.stderr)

    def test_base_requires_bundled_decoder_helpers(self):
        for name in ("media", "pdf"):
            with self.subTest(helper=name):
                helper = self.prefix / f"libexec/rummage/rummage-{name}"
                helper.unlink()
                self.assertNotEqual(self.run_check("off").returncode, 0)
                self.write(f"libexec/rummage/rummage-{name}", "fixture\n", True)

    def test_launcher_cannot_link_isolated_catalog_or_decoders(self):
        for library in ("libsqlite3.so", "librummage-photos.so", "libpdfium.so", "libavcodec.so",
                        "libavformat.so"):
            with self.subTest(library=library):
                self.env["RUMMAGE_TEST_NEEDED"] = f"(NEEDED) Shared library: [{library}]"
                result = self.run_check("off")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("directly links", result.stderr)
