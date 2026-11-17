#!/usr/bin/env python3
"""Render the full comparison table from raw paired benchmark results."""
import argparse
import json
from pathlib import Path
import statistics

from run import preview_metrics, trace


NAMES = ('rummage', 'nordend')


def median(values):
    return statistics.median(values) if values else None


def text(value, unit='', digits=1):
    return 'Not measured' if value is None else f'{value:,.{digits}f}{unit}'


def samples(report, name, fixture):
    return [r for r in report['live'] if r['app'] == name and r['fixture'] == fixture]


def mem(report, name, fixture, stage, scope='main', metric='rss_bytes'):
    # Equal weight per process; the three nearby readings are not independent runs.
    return median([median([s[scope][metric] / 1e6 for s in r[stage]])
                   for r in samples(report, name, fixture) if stage in r])


def memory_cell(report, name, fixture, stage):
    rss = mem(report, name, fixture, stage)
    pss = mem(report, name, fixture, stage, metric='pss_bytes')
    return f'{text(rss)} / {text(pss)} MB'


def startup(report, name, fixture, cold=False):
    return median([r['steps_ms']['folder-shown'] for r in report['startup']
                   if r['app'] == name and r['fixture'] == fixture and not r['warmup']
                   and bool(r.get('cold')) == cold])


def gaps(report, name):
    return [gap for r in samples(report, name, 'files10000') for gap in r['scroll']['gaps_ms']['samples']]


def percentile(values, percent):
    return sorted(values)[max(0, (percent * len(values) + 99) // 100 - 1)] if values else None


def preview(report, name, fixture):
    values = [v for r in samples(report, name, fixture) for v in (r['preview']['latency_ms'] or {}).get('samples', [])]
    return text(median(values), ' ms')


def render(report, path):
    rows = []
    startup_counts = sorted({sum(r['app'] == n and r['fixture'] == f and not r['warmup'] and not r.get('cold') for r in report['startup']) for n in NAMES for f in ('files20', 'files10000')})
    trial_counts = sorted({len(samples(report, n, f)) for n in NAMES for f in ('files20', 'files10000', 'images30', 'c-small', 'c-large')})
    cold_note = ('Cold startup uses the recorded explicit cache-reset hook; inspect its scope before interpreting these samples.'
                 if any(r.get('cold') for r in report['startup']) else
                 'True cold-cache startup was not run. The harness accepts an explicit `--cold-cache-hook` for a disposable test host; it does not label fresh-process starts as cold.')
    def row(label, fn):
        rows.append('| ' + label + ' | ' + ' | '.join(fn(name) for name in NAMES) + ' |')
    row('Stripped executable', lambda n: text(report['apps'][n]['stripped_bytes'] / 1e6, ' MB', 2))
    def libraries(n):
        counts = sorted({len(r['loaded_libraries']) for r in samples(report, n, 'files20')})
        return f"{len(report['apps'][n]['direct_libraries'])} / " + ', '.join(map(str, counts))
    row('Libraries, direct / loaded at idle', libraries)
    row('Warm start, 20 files', lambda n: text(startup(report, n, 'files20'), ' ms'))
    row('Warm start, 10,000 files', lambda n: text(startup(report, n, 'files10000'), ' ms'))
    row('Cold start, 20 files', lambda n: text(startup(report, n, 'files20', cold=True), ' ms'))
    for label, fixture in (('Idle memory, 20 files', 'files20'), ('Idle memory, 10,000 files', 'files10000'), ('Idle memory, 30 large PNGs', 'images30')):
        row(label, lambda n, f=fixture: memory_cell(report, n, f, 'idle_memory'))
    def cpu(n):
        measurements = ([r['measurement'] for r in report.get('idle_cpu_precise', []) if r['app'] == n]
                        or [r['idle_cpu'] for r in samples(report, n, 'files20')])
        value = median([r['percent_one_core'] for r in measurements])
        if measurements and 'cpu_ns' in measurements[0]:
            return text(value, '%', 4)
        resolution = max((r.get('tick_resolution_seconds', r.get('resolution_seconds', 0)) / r['seconds'] * 100 for r in measurements), default=0)
        return text(value, '%', 2) + f' (≈{resolution:.2f}% resolution)'
    row('Idle CPU, one-core percentage', cpu)
    row('Scrolling, median frame callback gap', lambda n: text(median(gaps(report, n)), ' ms', 2))
    row('Scrolling, p95 gap', lambda n: text(percentile(gaps(report, n), 95), ' ms', 2))
    row('Scrolling, worst gap', lambda n: text(max(gaps(report, n), default=None), ' ms', 2))
    def late(n):
        values = gaps(report, n)
        count = sum(v > 1.5 * report['frame_budget_ms'] for v in values)
        return f'{count} / {len(values):,} ({100 * count / len(values):.3f}%)' if values else 'Not measured'
    row('Scrolling, gaps >1.5× frame budget', late)
    row('Memory after scrolling', lambda n: memory_cell(report, n, 'files10000', 'after_scroll_memory'))
    row('PNG preview, key sent → settled callback', lambda n: preview(report, n, 'images30'))
    def missed(n):
        runs = samples(report, n, 'images30')
        sent = sum(r['preview']['sent'] for r in runs)
        delivery = sum(r['preview']['missed_delivery'] for r in runs)
        unadvanced = sum(r['preview'].get('unadvanced_selection', 0) for r in runs)
        wrong = sum(r['preview']['unsettled_or_wrong_selection'] for r in runs)
        return f'{delivery}/{sent} undelivered; {unadvanced} unadvanced; {wrong} unsettled/wrong'
    row('PNG preview, missed keys/selections', missed)
    row('Memory after 28 PNG transitions', lambda n: memory_cell(report, n, 'images30', 'after_preview_memory'))
    row('5,000-line C preview, key sent → settled callback', lambda n: preview(report, n, 'c-large'))
    row('Memory after 28 large C transitions', lambda n: memory_cell(report, n, 'c-large', 'after_preview_memory'))
    row('Copy 1 GB including destination sync', lambda n: text(median([r['seconds_including_sync'] for r in report['copy'] if r['app'] == n]), ' s', 3))
    cp = median([r['seconds_including_sync'] for r in report['copy'] if r['app'] == 'cp'])
    lines = ['# Local paired performance baseline', '',
             'Local paired measurements for the recorded builds, fixtures and rendering environment. Compare external results only when their workloads and measurement definitions match.', '',
             '**Memory cells are main-process RSS / PSS in decimal MB.** RSS counts every resident mapped page; PSS apportions shared pages. Neither includes the compositor or all GPU allocations. Helper memory is reported separately below. Each memory number is the median of process medians, not a pooled count of repeated reads.', '',
             '| Metric | Rummage | Nordend |', '|---|---:|---:|', *rows, '',
             f'Plain `cp --reflink=never --sparse=never`, with the same sync boundary: **{text(cp, " s", 3)}**.', '',
             '## C-preview memory, separated by workload', '',
             '| Workload | App | Main RSS | Main PSS | Main private | Helper PSS |', '|---|---|---:|---:|---:|---:|']
    stages = [('Unselected, 30 C files', 'c-large', 'idle_memory'),
              ('First 5,000-line C sidebar preview', 'c-large', 'first_preview_memory'),
              ('After 28 large C sidebar transitions', 'c-large', 'after_preview_memory'),
              ('Large C Quick Look open', 'c-large', 'quicklook_memory'),
              ('After 28 small (30-line) C transitions', 'c-small', 'after_preview_memory')]
    for label, fixture, stage in stages:
        for name in NAMES:
            rss = mem(report, name, fixture, stage)
            pss = mem(report, name, fixture, stage, metric='pss_bytes')
            private = mem(report, name, fixture, stage, metric='private_bytes')
            helpers = median([median([(s['tree']['pss_bytes'] - s['main']['pss_bytes']) / 1e6 for s in r[stage]])
                              for r in samples(report, name, fixture) if stage in r])
            lines.append(f'| {label} | {name} | {text(rss)} MB | {text(pss)} MB | {text(private)} MB | {text(helpers)} MB |')
    lines += ['', '## Preview ready versus animation finished', '',
              '| Endpoint | Rummage | Nordend |', '|---|---:|---:|']
    for fixture, label in (('images30', 'PNG'), ('c-large', 'Large C')):
        values = []
        for name in NAMES:
            loaded = [value for row in samples(report, name, fixture)
                      for value in (row['preview'].get('loaded_latency_ms') or {}).get('samples', [])]
            values.append(text(median(loaded), ' ms'))
        lines.append(f'| {label}: loaded callback, animation may continue | ' + ' | '.join(values) + ' |')
        lines.append(f'| {label}: loaded and animation finished | ' + ' | '.join(preview(report, name, fixture) for name in NAMES) + ' |')
    lines += ['', 'The gap between these endpoints is animation time, not additional preview loading. Both use the expected selected filename and a compositor callback; neither is a physical-presentation timestamp.']
    lines += ['', 'Live-window memory and offscreen software-renderer memory describe different workloads. Keep their results separate and record the rendering backend and process scope for each comparison.', '',
              'Sidebar content limits can differ between applications and revisions; verify them in the recorded sources. Quick Look is a separate full-preview workload. The C fixture is synthetic; results need not generalize to other files.', '',
              '## Method and coverage', '',
              '- Build settings, source diffs and executable hashes are recorded with the results.',
              '- Same toolchain, fixtures, icon theme, logical window size and graphics backend (see the recorded renderer logs). A private hardware-rendered headless Umbriel display runs at 120 Hz, with no desktop portal/system bus integrations.',
              f'- Startup uses uninstrumented executables: {startup_counts} measured launches per application/directory after a discarded warm-up pair. Endpoint: the application’s `folder-shown` frame callback. Fresh state/config and warm per-application shader/thumbnail caches.',
              f'- Live tests use {trial_counts} fresh-process trials per application/workload, following a discarded warm-up process. Both applications use identical temporary telemetry probes; probe overhead is not separately subtracted. Memory has three nearby readings per process; raw readings and helper process identities remain in JSON.',
              '- Scrolling uses continuous virtual pointer input for the configured duration; actual input durations and event counts are retained per trial. The first and last 0.5 seconds are excluded from gap statistics. Gaps are pooled across trials; worst values depend on sample count and desktop load.',
              '- Preview input uses the compositor’s virtual keyboard: 28 Down transitions per trial at 400 ms spacing. A settled result requires the expected filename, no pending loading, and no animation in the draw acknowledged by a frame callback. Missing delivery, unchanged selection, and missing settled results are distinct counters.',
              '- Frame callbacks measure compositor pacing, not physical scanout or input-to-photon latency. Compare preview latency only with measurements using the same settling endpoint.',
              '- ' + cold_note,
              '- Copy uses a fully allocated, deterministic 1,000,000,000-byte fixture on the recorded disk filesystem. Source cache is warmed equally. Timing includes process launch, copy, destination file fsync and destination directory fsync; SHA-256 verification is outside timing. Native application copy optimizations remain enabled.', '',
              '## Revisions and raw evidence', '']
    for name in NAMES:
        app = report['apps'][name]
        lines += [f'- **{name}**: `{app["revision"]}`; executable SHA-256 `{app["binary_sha256"]}`.']
    if report.get('idle_cpu_source'):
        lines += ['', 'Idle CPU was repeated using the high-resolution POSIX process CPU clock, including every main-process thread. See [raw idle CPU samples](' + report['idle_cpu_source'] + '). The original tick-based samples remain in the main results.']
    lines += ['', f'Machine: `{report["machine"]["machine"]}` / `{report["machine"]["release"]}`.', '',
              'Renderer logs, loaded library paths, CMake configuration, source diffs, raw memory samples, timing samples, input traces and selection/frame traces are retained alongside [results.json](results.json). Per-trial values should be examined before attributing small differences to application code.', '']
    path.write_text('\n'.join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('results', type=Path)
    p.add_argument('--out', type=Path)
    p.add_argument('--idle-results', type=Path, help='Optional separate high-resolution idle CPU run')
    a = p.parse_args()
    report = json.loads(a.results.read_text())
    for row in report['live']:
        if 'preview' not in row or 'loaded_latency_ms' in row['preview']:
            continue
        events = trace(Path(row['directory']) / 'trace.txt')
        if events:
            prefix, suffix = ('image', 'png') if row['fixture'] == 'images30' else ('sample', 'c')
            sent = [d['sent_ns'] for d in row['preview']['transitions']]
            row['preview'] = preview_metrics(sent, events, prefix, suffix, events[-1]['ns'] + 1)
    if a.idle_results:
        report['idle_cpu_precise'] = json.loads(a.idle_results.read_text())['idle_cpu_precise']
        report['idle_cpu_source'] = str(a.idle_results.resolve())
    render(report, a.out or a.results.with_name('report.md'))


if __name__ == '__main__':
    main()
