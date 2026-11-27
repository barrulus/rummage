#!/usr/bin/env python3
"""Prepare exact-tier private thumbnail seeds for the matched SSHFS benchmark.

Default is a metadata-only plan: no source media stat/read or thumbnail subprocesses.
Use --run only after the parent has completed the active UI measurement suite.
Originals are only stat'ed/read. All writes are under --out; CLI concurrency <= 4.
Reports contain counts and hashes, never original paths or filenames.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import time
import urllib.parse
import zlib


def digest(data):
    return hashlib.sha256(data).hexdigest()


def json_write(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def png_check(path, job):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('signature')
    at = 8
    tags = {}
    width = height = None
    ended = False
    while at + 12 <= len(data):
        length = struct.unpack('>I', data[at:at + 4])[0]
        kind = data[at + 4:at + 8]
        end = at + 8 + length
        if end + 4 > len(data):
            raise ValueError('truncated')
        payload = data[at + 8:end]
        expected_crc = struct.unpack('>I', data[end:end + 4])[0]
        if zlib.crc32(kind + payload) & 0xffffffff != expected_crc:
            raise ValueError('crc')
        if kind == b'IHDR':
            width, height = struct.unpack('>II', payload[:8])
        elif kind == b'tEXt':
            key, value = payload.split(b'\0', 1)
            tags[key.decode()] = value.decode()
        elif kind == b'IEND':
            ended = True
            if end + 4 != len(data):
                raise ValueError('trailing')
            break
        at = end + 4
    limit = {'large': 256, 'x-large': 512}[job['tier']]
    if not ended or width is None or height is None or not 0 < max(width, height) <= limit:
        raise ValueError('dimensions')
    if tags.get('Thumb::URI') != job['uri'] or tags.get('Thumb::MTime') != str(job['seconds']):
        raise ValueError('metadata')
    return dict(png_sha256=digest(data), bytes=len(data), width=width, height=height)


def prepare(args):
    rows = []
    with sqlite3.connect(args.catalog.resolve().as_uri() + '?mode=ro', uri=True) as db:
        for photo_id, source, relative, seconds, fraction, size in db.execute(
                "SELECT p.id,s.path,p.relative_path,p.mtime,p.mtime_ns,p.size "
                "FROM photos p JOIN sources s ON s.id=p.source_id "
                "WHERE s.active=1 AND p.availability<>'replaced' "
                "ORDER BY p.filename COLLATE NOCASE,p.id"):
            path = str(Path(source) / relative)
            uri = 'file://' + urllib.parse.quote(path, safe="/!$&'()*+,-.:=@_~")
            rows.append(dict(photo_id=photo_id, path=path, uri=uri, seconds=seconds,
                             modified_ns=seconds * 1_000_000_000 + fraction, size=size,
                             path_sha256=digest(path.encode()),
                             png_name=hashlib.md5(uri.encode()).hexdigest() + '.png'))
    events = [line.split() for line in args.trace.read_text().splitlines()]
    keys = [event for event in events if event[0] == 'key' and int(event[3]) in (1, 3, 5)]
    columns = {int(event[6]) for event in events if event[0] == 'draw' and int(event[3]) in (1, 3, 5)}
    if columns != {3}:
        raise ValueError('expected three-column traversal')
    by_phase = {}
    for event in keys:
        cursor = int(event[4])
        if f"photo:{rows[cursor]['photo_id']}" != event[5]:
            raise ValueError('catalog ordering differs from traced cursor identity')
        by_phase.setdefault(int(event[3]), []).append(cursor)
    if set(by_phase) != {1, 3, 5}:
        raise ValueError('expected three complete traversals')
    first = by_phase[1]
    if len(first) != 601 or first != list(range(0, 1801, 3)):
        raise ValueError('expected Home plus 600 Down inputs')
    if any(cursors != first for cursors in by_phase.values()):
        raise ValueError('traversals differed')
    # Grid padding exposes a sliver of the next row after the selected last row.
    # Existing completed remote cache confirms indices 1803..1805 were requested.
    grid = rows[:max(first) + 6]
    observed_large = args.trace.parent / 'cache/thumbnails/large'
    observed_indices = [i for i, row in enumerate(rows) if (observed_large / row['png_name']).exists()]
    if observed_indices and max(observed_indices) >= len(grid):
        raise ValueError('observed grid cache exceeds planned warm range')
    selected = [rows[cursor] for cursor in first]
    if len({row['path'] for row in grid}) != 1806 or len({row['path'] for row in selected}) != 601:
        raise ValueError('source paths are not unique')
    jobs = [dict(row, tier=tier) for tier, group in [('large', grid), ('x-large', selected)] for row in group]
    manifest = [{key: job[key] for key in ('photo_id', 'path_sha256', 'png_name', 'modified_ns', 'size', 'tier')}
                for job in jobs]
    plan = dict(schema=1, workers=args.workers, large_count=len(grid), x_large_count=len(selected),
                down_targets=600, initial_home_targets=1, total_pngs=len(jobs),
                grid_cursor_first=0, grid_cursor_last=1805,
                observed_grid_cache_max_index=max(observed_indices) if observed_indices else None,
                grid_coverage='Three-column cursor traversal plus one partially visible padding row; confirmed by completed remote cache entries at indices 1803..1805',
                order='large first, x-large second, so larger-tier cache reuse cannot suppress lower-tier persistence',
                trace_sha256=digest(args.trace.read_bytes()), catalog_sha256=digest(args.catalog.read_bytes()),
                helper_sha256=digest(args.thumb.read_bytes()),
                jobs_sha256=digest(json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()),
                source_validation='Catalog fractional nanoseconds combined with seconds; pre/post stat size+mtime, no original-content hashes',
                original_names_in_report=False, jobs=manifest)
    return jobs, plan


def execute(job, args, cache):
    path = Path(job['path'])
    target = cache / job['tier'] / job['png_name']
    result = {key: job[key] for key in ('tier', 'path_sha256', 'png_name')}
    started = time.monotonic()
    try:
        before = path.stat()
        if (before.st_size, before.st_mtime_ns) != (job['size'], job['modified_ns']):
            raise ValueError('source-metadata-mismatch-before')
        try:
            details = png_check(target, job)
            reused = True
        except (OSError, ValueError, UnicodeError, struct.error):
            reused = False
            completed = subprocess.run(
                [str(args.thumb), '--cache', str(cache), '--size', job['tier'], str(path)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout, check=False)
            result['cli_returncode'] = completed.returncode
            result['cli_output_sha256'] = digest(completed.stdout + completed.stderr)
            if completed.returncode:
                raise ValueError('thumbnail-cli-failed')
            details = png_check(target, job)
        after = path.stat()
        if (after.st_size, after.st_mtime_ns) != (job['size'], job['modified_ns']):
            raise ValueError('source-metadata-mismatch-after')
        result.update(status='ok', reused=reused, **details)
    except subprocess.TimeoutExpired:
        result.update(status='failed', reason='thumbnail-cli-timeout')
    except OSError as error:
        result.update(status='failed', reason='io-error', errno=error.errno)
    except (ValueError, UnicodeError, struct.error) as error:
        # Exception details are not reported because libraries can include original names.
        result.update(status='failed', reason='validation-or-cli-error', error_type=type(error).__name__)
    result['elapsed_s'] = round(time.monotonic() - started, 4)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--thumb', type=Path, default=Path('/tmp/rummage-remediation-build/rummage-thumb'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4, choices=range(1, 5))
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--run', action='store_true', help='Execute only after the active measurement suite finishes')
    args = parser.parse_args()
    args.out = args.out.absolute()
    if not args.out.is_relative_to(Path('/tmp')):
        parser.error('--out must be a private directory below /tmp')
    args.out.mkdir(mode=0o700, parents=True, exist_ok=True)
    jobs, plan = prepare(args)
    json_write(args.out / 'plan.json', plan)
    summary = {key: value for key, value in plan.items() if key != 'jobs'}
    if not args.run:
        print(json.dumps(dict(summary, status='prepared-not-run'), indent=2))
        return 0
    (args.out / 'READY.json').unlink(missing_ok=True)
    cache = args.out / 'cache'
    cache.mkdir(mode=0o700, exist_ok=True)
    started = time.monotonic()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for tier in ('large', 'x-large'):
            group = [job for job in jobs if job['tier'] == tier]
            futures = [pool.submit(execute, job, args, cache) for job in group]
            for completed in concurrent.futures.as_completed(futures):
                results.append(completed.result())
                if len(results) % 100 == 0 or len(results) == len(jobs):
                    progress = dict(completed=len(results), total=len(jobs),
                                    failed=sum(row['status'] != 'ok' for row in results),
                                    elapsed_s=round(time.monotonic() - started, 1))
                    json_write(args.out / 'progress.json', progress)
                    print(json.dumps(progress), flush=True)
    # Revalidate all expected exact PNGs after generation, independent of CLI stdout claims.
    verified = []
    final_errors = []
    for job in jobs:
        try:
            details = png_check(cache / job['tier'] / job['png_name'], job)
            verified.append(dict(tier=job['tier'], name=job['png_name'], **details))
        except (OSError, ValueError, UnicodeError, struct.error):
            final_errors.append(dict(tier=job['tier'], path_sha256=job['path_sha256']))
    failed = sum(row['status'] != 'ok' for row in results)
    verified.sort(key=lambda row: (row['tier'], row['name']))
    summary.update(status='ready' if not failed and not final_errors else 'failed',
                   completed=len(results), failed=failed, verified_exact_pngs=len(verified),
                   verification_failures=final_errors, elapsed_s=round(time.monotonic() - started, 2),
                   total_png_bytes=sum(row['bytes'] for row in verified),
                   seed_manifest_sha256=digest(json.dumps(verified, sort_keys=True, separators=(',', ':')).encode()))
    json_write(args.out / 'results.json', results)
    json_write(args.out / 'png-manifest.json', verified)
    json_write(args.out / 'summary.json', summary)
    if summary['status'] == 'ready':
        json_write(args.out / 'READY.json', summary)
    print(json.dumps(summary, indent=2), flush=True)
    return 0 if summary['status'] == 'ready' else 1


if __name__ == '__main__':
    raise SystemExit(main())
