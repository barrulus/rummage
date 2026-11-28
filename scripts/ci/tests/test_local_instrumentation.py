"""Local sanitizer runs reuse only their own builds and always create fresh reports."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "local_instrumentation", Path(__file__).resolve().parents[1] / "local-instrumentation.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LocalInstrumentationTest(unittest.TestCase):
    def test_checkout_paths_are_separate_but_share_pinned_toolchain_profile(self):
        cache = Path("/cache")
        first = module.paths(cache, Path("/one/checkout"))
        second = module.paths(cache, Path("/another/checkout"))
        self.assertEqual(first[0], second[0])
        self.assertNotEqual(first[1], second[1])

    def test_repeated_runs_reuse_builds_but_never_overwrite_reports(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()), \
             patch.object(module.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            for _ in range(2):
                self.assertEqual(module.main(["asan", "--cache-dir", directory, "--jobs", "8"]), 0)
            first, second = (call.args[0] for call in run.call_args_list)
            self.assertEqual(first[first.index("--build-dir") + 1],
                             second[second.index("--build-dir") + 1])
            self.assertNotEqual(first[first.index("asan") + 1], second[second.index("asan") + 1])
            self.assertEqual(first[first.index("--profile") + 1], str(Path(directory) / "toolchain"))
            self.assertIn(".#instrumentation", first)

    def test_all_stops_after_failure_before_other_profiles(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()), \
             patch.object(module.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)) as run:
            self.assertEqual(module.main(["all", "--cache-dir", directory]), 1)
            self.assertEqual(run.call_count, 1)
