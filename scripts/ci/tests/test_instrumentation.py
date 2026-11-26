"""Check bounded execution and reporting without building packages in tooling tests."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "instrumentation", Path(__file__).resolve().parents[1] / "instrumentation.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InstrumentationTest(unittest.TestCase):
    def test_profiles_instrument_distinct_builds_and_require_optional_engines(self):
        asan = module.configure("asan", Path("build"))
        self.assertIn("-DRUMMAGE_SANITIZER=address,undefined", asan)
        self.assertIn("-DRUMMAGE_MEDIA=ON", asan)
        self.assertIn("-DRUMMAGE_PDF=ON", asan)
        self.assertIn("-DRUMMAGE_BUILD_TESTS=ON", asan)
        self.assertIn("-DRUMMAGE_SANITIZER=thread", module.configure("tsan", Path("build")))
        fuzz = module.configure("fuzz", Path("build"))
        self.assertIn("-DRUMMAGE_FUZZ=ON", fuzz)
        self.assertNotIn("-DRUMMAGE_SANITIZER=thread", fuzz)

    def test_fuzz_limits_and_reproducers_are_per_target(self):
        for target in module.FUZZERS:
            command = module.fuzz_command(target, Path("build"), Path("output"), 15)
            self.assertIn("-max_total_time=15", command)
            self.assertIn("-timeout=10", command)
            self.assertIn("-rss_limit_mb=2048", command)
            self.assertIn(f"-artifact_prefix=output/failures/{target}/", command)

    def test_configuration_failure_retains_report_and_log(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            # Use a real failing subprocess to exercise log retention and return codes.
            with patch.object(module, "configure", return_value=["sh", "-c", "echo broken; exit 7"]), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(module.main(["asan", str(output)]), 1)
            report = json.loads((output / "summary.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["commands"][0]["returncode"], 7)
            self.assertIn("broken", (output / "configure.log").read_text())

    def test_output_cannot_overwrite_evidence(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                module.main(["fuzz", directory])

    def test_all_instrumented_jobs_gate_normal_tests(self):
        workflow = (module.ROOT / ".github/workflows/ci.yml").read_text()
        self.assertIn("profile: [asan, tsan, fuzz]", workflow)
        self.assertIn("needs: [reuse, website, instrumentation]", workflow)
        job = workflow.split("  instrumentation:\n", 1)[1].split("  tests:\n", 1)[0]
        self.assertIn("if: always()", job)
        self.assertNotIn("continue-on-error", job)
