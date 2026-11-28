#!/usr/bin/env python3
"""Check benchmark-only glibc sampling and reclamation on a disposable process.

Run inside `nix develop`, optionally with --out NEW-DIRECTORY to retain evidence.
This deliberately creates ~128 MiB of touched allocations, frees all but the last,
then asks the event-loop-style probe to trim. It never attaches to another process.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


SOURCE = r'''
#include HEADER
#include <array>
#include <cstring>

void command(const char* text) {
    FILE* file = std::fopen(std::getenv("ALLOCATOR_CONTROL"), "w");
    if (!file || std::fputs(text, file) < 0 || std::fclose(file) != 0)
        std::exit(3);
    allocator_probe::poll();
}

int main() {
    mallopt(M_MMAP_THRESHOLD, 16 << 20);
    allocator_probe::sample("baseline", "sample");
    std::array<void*, 64> blocks{};
    for (auto& block : blocks) {
        block = std::malloc(2 << 20);
        if (!block)
            return 2;
        std::memset(block, 0xa5, 2 << 20);
    }
    allocator_probe::sample("allocated", "sample");
    for (std::size_t i = 0; i < blocks.size() - 1; ++i)
        std::free(blocks[i]);
    allocator_probe::sample("freed", "sample");
    command("trim settled\n");
    allocator_probe::poll(); // consumed commands must not execute twice
    command("sample invalid\"tag\n"); // JSON-unsafe tag must not be emitted
    command("sample\n"); // incomplete command must not be emitted
    command("unknown ignored\n");
    command("sample final\n");
    std::free(blocks.back());
}
'''


def check(root, compiler):
    header = Path(__file__).with_name('allocator_probe.hpp').resolve()
    source = root / 'selftest.cpp'
    source.write_text(SOURCE.replace('HEADER', json.dumps(str(header))))
    executable = root / 'selftest'
    compile_command = [*shlex.split(compiler), '-Wall', '-Wextra', '-Werror', '-O2',
                       str(source), '-o', str(executable)]
    subprocess.run(compile_command, check=True, timeout=60)
    env = dict(os.environ, ALLOCATOR_TRACE=str(root / 'samples.jsonl'),
               ALLOCATOR_CONTROL=str(root / 'control'))
    subprocess.run([str(executable)], env=env, check=True, timeout=30)
    rows = [json.loads(line) for line in (root / 'samples.jsonl').read_text().splitlines()]
    assert [(row['tag'], row['stage']) for row in rows] == [
        ('baseline', 'sample'), ('allocated', 'sample'), ('freed', 'sample'),
        ('settled', 'before_trim'), ('settled', 'after_trim'), ('final', 'sample')]
    assert not (root / 'control').exists(), 'Control command was not consumed'
    baseline, allocated, freed, before, after, final = rows
    assert len({row['pid'] for row in rows}) == 1
    assert all(left['time_ns'] <= right['time_ns'] for left, right in zip(rows, rows[1:]))
    assert allocated['live_bytes'] - baseline['live_bytes'] >= 128 * 1024**2
    assert allocated['live_bytes'] - freed['live_bytes'] >= 125 * 1024**2
    assert before['live_bytes'] == after['live_bytes'], 'Trim changed live accounting'
    assert before['rss_bytes'] - after['rss_bytes'] >= 64 * 1024**2
    assert after['trim_result'] == 1
    assert final['live_bytes'] == after['live_bytes']
    result = {'status': 'passed', 'samples': len(rows),
              'rss_reclaimed_bytes': before['rss_bytes'] - after['rss_bytes'],
              'free_chunks_after_trim_bytes': after['free_bytes'],
              'note': 'Free chunk address space can remain after its resident pages are reclaimed.',
              'compile_command': compile_command}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.out:
        args.out.mkdir(parents=True, exist_ok=False)
        check(args.out.resolve(), args.cxx)
    else:
        with tempfile.TemporaryDirectory(prefix='rummage-allocator-selftest-') as directory:
            check(Path(directory), args.cxx)


if __name__ == '__main__':
    main()
