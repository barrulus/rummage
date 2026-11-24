"""Keep the package graph behind the unconditional test gate (actionlint checks YAML)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WorkflowGateTest(unittest.TestCase):
    def setUp(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text().split('\njobs:\n', 1)[1]
        self.jobs = dict(re.findall(r'^  (\w+):\n(.*?)(?=^  \w+:|\Z)', workflow, re.M | re.S))

    def dependencies(self, job):
        match = re.search(r'^    needs: (.+)$', self.jobs[job], re.M)
        return set(match[1].strip('[]').replace(',', ' ').split()) if match else set()

    def test_every_package_job_requires_successful_tests(self):
        for job in ('nix', 'void', 'native'):
            with self.subTest(job=job):
                self.assertIn('tests', self.dependencies(job))
                header = self.jobs[job].split('    steps:', 1)[0]
                self.assertNotIn('always()', header)
                self.assertNotIn('!cancelled()', header)
                self.assertNotIn('continue-on-error:', header)

    def test_tests_are_not_skipped_for_manual_or_reused_builds(self):
        header = self.jobs['tests'].split('    steps:', 1)[0]
        self.assertNotIn('    if:', header)
        self.assertNotIn('continue-on-error:', self.jobs['tests'])
        self.assertIn('run: python3 scripts/ci/preflight.py lint', self.jobs['tests'])
        self.assertIn('run: python3 scripts/ci/preflight.py tests', self.jobs['tests'])
        self.assertNotIn('run: python3 scripts/ci/preflight.py nix', self.jobs['tests'])

    def test_download_preparation_requires_successful_tests(self):
        self.assertIn('tests', self.dependencies('downloads'))
        self.assertIn("needs.tests.result == 'success'", self.jobs['downloads'])
