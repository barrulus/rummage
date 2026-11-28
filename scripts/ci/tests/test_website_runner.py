import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


RUNNER = Path(__file__).resolve().parents[1] / 'website.sh'


class WebsiteRunnerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.calls = self.root / 'calls'
        archive = self.root / 'hugo.tar.gz'
        hugo = b'#!/bin/sh\nprintf "hugo %s\\n" "$*" >> "$WEBSITE_TEST_CALLS"\nexit "${WEBSITE_TEST_BUILD_EXIT:-0}"\n'
        with tarfile.open(archive, 'w:gz') as bundle:
            info = tarfile.TarInfo('hugo')
            info.size = len(hugo)
            info.mode = 0o755
            bundle.addfile(info, io.BytesIO(hugo))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.script = self.root / 'scripts/ci/website.sh'
        self.script.parent.mkdir(parents=True)
        script = RUNNER.read_text()
        old_digest = next(line for line in script.splitlines() if line.startswith('HUGO_SHA256='))
        self.script.write_text(script.replace(old_digest, f"HUGO_SHA256='{digest}'"))
        website = self.root / 'website'
        website.mkdir()
        (website / 'check.py').write_text(
            'import os, pathlib, sys\n'
            'with pathlib.Path(os.environ["WEBSITE_TEST_CALLS"]).open("a") as log:\n'
            '    log.write("check " + sys.argv[1] + "\\n")\n'
            'sys.exit(int(os.environ.get("WEBSITE_TEST_CHECK_EXIT", "0")))\n')
        binary = self.root / 'bin'
        binary.mkdir()
        curl = binary / 'curl'
        curl.write_text('#!/bin/sh\nwhile [ "$1" != "--output" ]; do shift; done\n'
                        'cp "$WEBSITE_TEST_ARCHIVE" "$2"\n')
        curl.chmod(0o755)
        self.env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                        WEBSITE_TEST_ARCHIVE=str(archive), WEBSITE_TEST_CALLS=str(self.calls))

    def run_runner(self, **env):
        return subprocess.run(['bash', str(self.script), str(self.root / 'public')],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_build_and_checker_share_clean_destination(self):
        result = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text().splitlines()
        self.assertEqual(calls, [
            f'hugo --source website --destination {self.root / "public"} --cleanDestinationDir '
            '--minify --panicOnWarning --printPathWarnings',
            f'check {self.root / "public"}'])

    def test_checksum_failure_never_executes_download(self):
        Path(self.env['WEBSITE_TEST_ARCHIVE']).write_bytes(b'wrong archive')
        self.assertNotEqual(self.run_runner().returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_build_failure_skips_checker(self):
        self.assertEqual(self.run_runner(WEBSITE_TEST_BUILD_EXIT='19').returncode, 19)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)

    def test_checker_failure_is_a_failed_gate(self):
        self.assertEqual(self.run_runner(WEBSITE_TEST_CHECK_EXIT='23').returncode, 23)
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)
