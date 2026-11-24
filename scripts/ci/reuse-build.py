#!/usr/bin/env python3
"""Find a successful PR build of exactly the current main tree."""
import json
import os
import re
import subprocess

REQUIRED_JOBS = {
    'Tests and lint',
    'Nix packages and installed checks',
    'Void glibc package and clean install',
    'Debian 13 package and clean install',
    'Fedora 45 package and clean install',
    'Arch Linux package and clean install',
    'openSUSE Tumbleweed package and clean install',
    'Prepare development downloads',
}


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True))


def find_build(repository, tree, request=api):
    prefix = f'repos/{repository}'
    runs = request(f'{prefix}/actions/workflows/ci.yml/runs?event=pull_request&status=success&per_page=30')
    for run in runs['workflow_runs']:
        if run['event'] != 'pull_request' or run['conclusion'] != 'success':
            continue
        artifacts = request(f'{prefix}/actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
        names = {a['name'] for a in artifacts if not a['expired']}
        for name in sorted(names):
            match = re.fullmatch(r'development-downloads-([0-9a-f]{40})', name)
            if not match:
                continue
            commit = match[1]
            # Packages, the Nix closure and the test gate must share the tested checkout.
            if not {f'nix-store-{commit}', f'test-result-{commit}'} <= names:
                continue
            source = request(f'{prefix}/git/commits/{commit}')
            if source['tree']['sha'] != tree:
                continue
            # PR artifacts use the synthetic merge SHA, while run.head_sha is the PR head.
            if commit != run['head_sha'] and (
                    len(source['parents']) != 2 or source['parents'][1]['sha'] != run['head_sha']):
                continue
            jobs = request(f'{prefix}/actions/runs/{run["id"]}/jobs?per_page=100')['jobs']
            passed = {job['name'] for job in jobs if job['conclusion'] == 'success'}
            if not REQUIRED_JOBS <= passed:
                continue
            return str(run['id']), commit
    return '', ''


def main():
    run_id, commit = '', ''
    if os.environ['GITHUB_REF'] == 'refs/heads/main' and os.environ['GITHUB_EVENT_NAME'] == 'push':
        tree = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], text=True).strip()
        try:
            run_id, commit = find_build(os.environ['GITHUB_REPOSITORY'], tree)
        except (subprocess.CalledProcessError, KeyError, ValueError) as error:
            # Reuse is an optimization. A lookup failure must never bypass validation.
            print(f'Cannot establish reusable validation; building normally: {error}')
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write(f'run_id={run_id}\ncommit={commit}\n')
    print(f'Reusing validated build {run_id} ({commit})' if run_id else 'Building this checkout normally')


if __name__ == '__main__':
    main()
