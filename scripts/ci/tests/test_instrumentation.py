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
        self.assertIn("-DRUMMAGE_PHOTOS=ON", asan)
        self.assertIn("-DRUMMAGE_FONT=ON", asan)
        self.assertIn("-DRUMMAGE_MODEL=ON", asan)
        self.assertIn("-DRUMMAGE_TERMINAL=ON", asan)
        self.assertIn("-DRUMMAGE_BUILD_TESTS=ON", asan)
        self.assertIn("-DRUMMAGE_SANITIZER=thread", module.configure("tsan", Path("build")))
        fuzz = module.configure("fuzz", Path("build"))
        self.assertIn("-DRUMMAGE_FUZZ=ON", fuzz)
        self.assertIn("-DRUMMAGE_PHOTOS=OFF", fuzz)
        self.assertIn("-DRUMMAGE_PHOTOS=OFF", module.configure("tsan", Path("build")))
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

    def test_reusable_build_checks_source_profile_and_cmake_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            module.reusable_build(build, "asan").close()
            module.reusable_build(build, "asan").close()
            with self.assertRaisesRegex(ValueError, "another source or profile"):
                module.reusable_build(build, "tsan")
            (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/another/source\n")
            with self.assertRaisesRegex(ValueError, "CMake cache belongs"):
                module.reusable_build(build, "asan")
            unowned = Path(directory) / "unowned"
            unowned.mkdir()
            (unowned / "keep").write_text("existing work")
            with self.assertRaisesRegex(ValueError, "nonempty build"):
                module.reusable_build(unowned, "asan")
            self.assertEqual((unowned / "keep").read_text(), "existing work")

    def test_reusable_build_rejects_concurrent_use(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            with module.reusable_build(build, "asan"):
                with self.assertRaisesRegex(ValueError, "already in use"):
                    module.reusable_build(build, "asan")
            module.reusable_build(build, "asan").close()

    def test_runs_keep_separate_reports_when_reusing_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            with patch.object(module, "configure", return_value=["sh", "-c", "echo broken; exit 7"]), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                for number in (1, 2):
                    output = root / f"report-{number}"
                    self.assertEqual(module.main(["asan", str(output), "--build-dir", str(build)]), 1)
                    report = json.loads((output / "summary.json").read_text())
                    self.assertEqual(report["build_directory"], str(build))
                    self.assertIn("broken", (output / "configure.log").read_text())

    def test_failure_excerpt_keeps_assertion_and_summary_without_dumping_full_log(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "tests.log"
            log.write_text("noise\n" * 300 + "tests/example.cpp:12: FATAL ERROR: REQUIRE(false)\n"
                           + "values: false\n" + "noise\n" * 300 + "Status: FAILURE!\n")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                module.failure_excerpt(log)
            self.assertIn("REQUIRE(false)", stderr.getvalue())
            self.assertIn("values: false", stderr.getvalue())
            self.assertIn("Status: FAILURE!", stderr.getvalue())
            self.assertLess(len(stderr.getvalue().splitlines()), 110)

    def test_remote_instrumentation_is_an_explicit_manual_option(self):
        workflow = (module.ROOT / ".github/workflows/ci.yml").read_text()
        self.assertIn("profile: [asan, tsan, fuzz]", workflow)
        self.assertIn("needs: [reuse, website]", workflow)
        job = workflow.split("  instrumentation:\n", 1)[1].split("  tests:\n", 1)[0]
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.instrumentation == true", job)
        self.assertIn("if: always()", job)
        self.assertNotIn("continue-on-error", job)
