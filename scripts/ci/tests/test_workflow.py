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

    def test_instrumentation_is_manual_opt_in_and_not_a_normal_test_dependency(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        inputs = workflow.split('  workflow_dispatch:\n', 1)[1].split('\npermissions:', 1)[0]
        self.assertRegex(inputs, r'(?s)instrumentation:\n.*?type: boolean\n        default: false')
        header = self.jobs['instrumentation'].split('    steps:', 1)[0]
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.instrumentation == true", header)
        self.assertNotIn('instrumentation', self.dependencies('tests'))

    def test_requested_instrumentation_must_pass_before_download_publication(self):
        self.assertIn('instrumentation', self.dependencies('downloads'))
        gate = self.jobs['downloads'].split('    runs-on:', 1)[0]
        self.assertIn("!cancelled()", gate)  # Explicit status check allows the optional skipped job.
        self.assertIn("inputs.instrumentation == true && needs.instrumentation.result == 'success'", gate)
        self.assertIn("inputs.instrumentation != true && needs.instrumentation.result == 'skipped'", gate)
        self.assertEqual(self.dependencies('publish'), {'downloads'})

    def test_application_pipeline_requires_checked_website(self):
        self.assertIn('website', self.dependencies('tests'))
        website = self.jobs['website']
        self.assertNotIn('    if:', website.split('    steps:', 1)[0])
        self.assertNotIn('continue-on-error:', website)
        self.assertIn('run: bash scripts/ci/website.sh', website)
        self.assertNotIn('HUGO_SHA256:', website)
        runner = (ROOT / 'scripts/ci/website.sh').read_text()
        self.assertIn('sha256sum --check -', runner)
        self.assertIn('python3 website/check.py "$output"', runner)
        self.assertIn('path: website/public', website)

    def test_pages_requires_checked_site_but_not_application_release(self):
        self.assertEqual(self.dependencies('pages'), {'website'})
        header = self.jobs['pages'].split('    steps:', 1)[0]
        self.assertIn("github.repository == 'barrulus/rummage'", header)
        self.assertIn("github.ref == 'refs/heads/main'", header)
        self.assertIn('inputs.tumbleweed_only != true', header)
        self.assertNotIn('always()', header)
        self.assertNotIn('!cancelled()', header)
        self.assertFalse((ROOT / '.github/workflows/site.yml').exists())


class WorkflowTriggerTest(unittest.TestCase):
    def test_all_workflows_are_manual_except_main_pushes(self):
        for path in (ROOT / '.github/workflows').glob('*.yml'):
            with self.subTest(workflow=path.name):
                triggers = path.read_text().split('\non:\n', 1)[1].split('\npermissions:', 1)[0]
                events = set(re.findall(r'^  (\w+):', triggers, re.M))
                self.assertEqual(events, {'push', 'workflow_dispatch'})
                push = triggers.split('  push:\n', 1)[1]
                push = re.split(r'^  \w+:', push, maxsplit=1, flags=re.M)[0]
                self.assertRegex(push, r'(?m)^    branches: \[main\]$')
                self.assertNotIn('tags:', push)
