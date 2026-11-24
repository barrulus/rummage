import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    with patch.object(sys, 'path', [str(SCRIPTS), *sys.path]):
        spec.loader.exec_module(module)
    return module


reuse = load('reuse-build')
promote = load('promote-downloads')
nix_cache = load('nix-cache')
TESTED, HEAD, MERGED, TREE = (letter * 40 for letter in 'abcd')


class ReuseTest(unittest.TestCase):
    def setUp(self):
        self.run = {'id': 42, 'event': 'workflow_dispatch', 'conclusion': 'success',
                    'head_sha': TESTED, 'head_branch': 'feature'}
        self.artifacts = [{'name': f'{prefix}-{TESTED}', 'expired': False}
                          for prefix in ('development-downloads', 'nix-store', 'test-result')]
        self.source = {'tree': {'sha': TREE}, 'parents': [{'sha': 'e' * 40}, {'sha': HEAD}]}
        self.jobs = [{'name': name, 'conclusion': 'success'} for name in reuse.REQUIRED_JOBS]
        self.requests = []

    def request(self, path):
        self.requests.append(path)
        if '/workflows/' in path:
            return {'workflow_runs': [self.run]}
        if '/artifacts?' in path:
            return {'artifacts': self.artifacts}
        if '/git/commits/' in path:
            return self.source
        if '/jobs?' in path:
            return {'jobs': self.jobs}
        raise AssertionError(path)

    def find(self):
        return reuse.find_build('owner/repo', TREE, self.request)

    def test_identical_tree_accepts_different_merge_commit(self):
        self.assertEqual(self.find(), ('42', TESTED))
        self.assertTrue(all(path.startswith('repos/owner/repo/') for path in self.requests))

    def test_source_change_requires_rebuild(self):
        self.source['tree']['sha'] = 'f' * 40
        self.assertEqual(self.find(), ('', ''))

    def test_unrelated_artifact_commit_is_rejected(self):
        self.run['head_sha'] = HEAD
        self.assertEqual(self.find(), ('', ''))

    def test_missing_or_expired_artifacts_require_rebuild(self):
        for index in range(len(self.artifacts)):
            with self.subTest(index=index), patch.dict(self.artifacts[index], expired=True):
                self.assertEqual(self.find(), ('', ''))
        self.artifacts.pop()
        self.assertEqual(self.find(), ('', ''))

    def test_failed_or_skipped_required_job_cannot_be_promoted(self):
        for conclusion in ('failure', 'skipped', 'cancelled', None):
            with self.subTest(conclusion=conclusion), patch.dict(self.jobs[0], conclusion=conclusion):
                self.assertEqual(self.find(), ('', ''))

    def test_missing_required_job_cannot_be_promoted(self):
        self.jobs.pop()
        self.assertEqual(self.find(), ('', ''))

    def test_build_without_successful_test_gate_cannot_be_reused(self):
        self.jobs = [job for job in self.jobs if job['name'] != 'Tests and lint']
        self.assertEqual(self.find(), ('', ''))

    def test_failed_or_non_manual_branch_run_cannot_be_promoted(self):
        for change in ({'conclusion': 'failure'}, {'event': 'push'},
                       {'event': 'pull_request'}, {'head_branch': 'main'}):
            with self.subTest(change=change), patch.dict(self.run, change):
                self.assertEqual(self.find(), ('', ''))

    def test_main_lookup_error_falls_back_to_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'output'
            with patch.dict(os.environ, GITHUB_REF='refs/heads/main', GITHUB_EVENT_NAME='push',
                            GITHUB_REPOSITORY='owner/repo', GITHUB_OUTPUT=str(output)), \
                    patch.object(reuse.subprocess, 'check_output', return_value=TREE), \
                    patch.object(reuse, 'find_build', side_effect=subprocess.CalledProcessError(1, 'gh')):
                reuse.main()
            self.assertEqual(output.read_text(), 'run_id=\ncommit=\n')

    def test_manual_runs_always_validate_normally(self):
        for event, ref in [('workflow_dispatch', 'refs/heads/feature'),
                           ('workflow_dispatch', 'refs/heads/main')]:
            with self.subTest(event=event), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / 'output'
                with patch.dict(os.environ, GITHUB_REF=ref, GITHUB_EVENT_NAME=event,
                                GITHUB_OUTPUT=str(output)), patch.object(reuse, 'find_build') as find:
                    reuse.main()
                find.assert_not_called()
                self.assertEqual(output.read_text(), 'run_id=\ncommit=\n')


class PromotionTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.identity = {'commit': TESTED, 'timestamp': '20261115000000', 'version': '0.5.1',
                         'sha256': hashlib.sha256(b'original source').hexdigest()}
        (self.root / 'source.json').write_text(json.dumps(self.identity))
        (self.root / 'rummage-source.tar.gz').write_bytes(b'original source')
        (self.root / 'package.deb').write_bytes(b'original binary')
        self.manifest = self.root / 'SHA256SUMS'
        self.manifest.write_text(''.join(
            f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in sorted(self.root.iterdir())))
        (self.root / 'release-notes.md').write_text('Original successful branch build link.\n')

    def promote(self, tree=TREE, tested_tree=TREE, commit=TESTED):
        promote.promote(self.root, commit, MERGED, tree, tested_tree)

    def test_promotion_keeps_tested_identity_and_binary_bytes(self):
        self.promote()
        identity = json.loads((self.root / 'source.json').read_text())
        self.assertEqual(identity, {**self.identity, 'published_commit': MERGED, 'tree': TREE})
        self.assertEqual((self.root / 'package.deb').read_bytes(), b'original binary')
        self.assertEqual((self.root / 'rummage-source.tar.gz').read_bytes(), b'original source')
        for line in self.manifest.read_text().splitlines():
            digest, name = line.split('  ')
            self.assertEqual(hashlib.sha256((self.root / name).read_bytes()).hexdigest(), digest)
        self.assertIn('Original successful branch build link', (self.root / 'release-notes.md').read_text())

    def test_changed_tree_and_wrong_commit_are_rejected_before_mutation(self):
        before = (self.root / 'source.json').read_bytes()
        for kwargs in ({'tested_tree': 'f' * 40}, {'commit': MERGED}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.promote(**kwargs)
            self.assertEqual((self.root / 'source.json').read_bytes(), before)

    def test_tampering_is_rejected(self):
        (self.root / 'package.deb').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.promote()

    def test_manifest_path_traversal_is_rejected(self):
        with self.manifest.open('a') as manifest:
            manifest.write('a' * 64 + '  ../outside\n')
        with self.assertRaisesRegex(ValueError, 'filename'):
            self.promote()


class NixTransferTest(unittest.TestCase):
    def test_roots_include_all_outputs_of_packages_and_checks(self):
        def evaluate(command, **kwargs):
            self.assertEqual(command[:3], ['nix', 'eval', '--json'])
            self.assertIn('p.outputs', command[-1])
            return json.dumps([f'/nix/store/{command[3]}-out', f'/nix/store/{command[3]}-helper'])
        with patch.object(nix_cache.subprocess, 'check_output', side_effect=evaluate):
            paths = nix_cache.roots()
        self.assertEqual(len(paths), 14)
        self.assertTrue(any('checks.x86_64-linux.smoke' in path for path in paths))

    def test_test_result_transfer_does_not_evaluate_or_copy_packages(self):
        for mode in ('export', 'restore'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                calls = []
                with patch.object(nix_cache.subprocess, 'check_output',
                                  return_value=json.dumps(['/nix/store/test-result'])) as evaluate:
                    directory = Path(temporary) / 'cache'
                    nix_cache.transfer(mode, directory, tests_only=True,
                                       run=lambda cmd, **kw: calls.append(cmd))
                evaluate.assert_called_once()
                self.assertEqual(evaluate.call_args.args[0][3], nix_cache.NIX_TESTS)
                self.assertEqual(calls, [[
                    'nix', 'copy', '--to' if mode == 'export' else '--from', directory.as_uri(),
                    *([] if mode == 'export' else ['--no-check-sigs']), '/nix/store/test-result']])

    def test_restore_uses_locally_evaluated_roots(self):
        paths = ['/nix/store/expected-out', '/nix/store/expected-media']
        with patch.object(nix_cache, 'roots', return_value=paths):
            calls = []
            nix_cache.transfer('restore', Path('/tmp/cache'), run=lambda cmd, **kw: calls.append(cmd))
        self.assertEqual(calls, [['nix', 'copy', '--from', 'file:///tmp/cache', '--no-check-sigs', *paths]])

    def test_evaluation_failure_prevents_import_or_upload(self):
        for mode in ('export', 'restore', 'push'):
            with self.subTest(mode=mode), patch.object(nix_cache, 'roots', side_effect=ValueError('bad root')):
                calls = []
                with self.assertRaises(ValueError):
                    nix_cache.transfer(mode, Path('/tmp/cache'), run=lambda cmd, **kw: calls.append(cmd))
                self.assertEqual(calls, [])
