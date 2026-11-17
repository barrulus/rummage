#!/usr/bin/env python3
"""Paired Linux/Wayland performance measurements of live Rummage and Nordend.

Requires separately built Release binaries and identical benchmark probes made by
instrument.py. Uses a dedicated compositor and real virtual keyboard/pointer
input. Raw samples, environment, source diffs and binary hashes accompany results.
No measurements in this program are offscreen snapshot memory measurements.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import struct
import subprocess
import sys
import time
import zlib


def command(args, **kwargs):
    return subprocess.check_output([str(x) for x in args], text=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write_once(path, data):
    if not path.exists():
        path.write_bytes(data)


def png(width, height, seed):
    # Deterministic textured RGB image, not a one-colour decoder fast path.
    def chunk(kind, payload):
        return struct.pack('!I', len(payload)) + kind + payload + struct.pack('!I', zlib.crc32(kind + payload))
    compressor = zlib.compressobj(3)
    encoded = []
    for y in range(height):
        row = bytes(v for x in range(width) for v in
                    ((x * 7 + y + seed) % 256, (x + y * 3 + seed * 13) % 256,
                     ((x ^ y) + seed * 31) % 256))
        encoded.append(compressor.compress(b'\0' + row))
    encoded.append(compressor.flush())
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', b''.join(encoded)) + chunk(b'IEND', b''))


def fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    for name in ('files20', 'files10000', 'images30', 'c-small', 'c-large'):
        (root / name).mkdir(exist_ok=True)
    for name, count in (('files20', 20), ('files10000', 10000)):
        for i in range(count):
            write_once(root / name / f'file-{i:05}.txt', f'Fixture {i}\n'.encode())
    for name, lines in (('c-small', 30), ('c-large', 5000)):
        for i in range(30):
            write_once(root / name / f'sample-{i:05}.c', ''.join(
                f'int function_{n:05}(int x) {{ return x + {n + i:05}; }}\n'
                for n in range(lines)).encode())
    # Decode footprint is 36 MB per image. Separate files exercise cache turnover.
    # Vary PNG metadata while sharing pixels to keep fixture creation inexpensive.
    first = root / 'images30/image-00000.png'
    if not first.exists():
        first.write_bytes(png(4000, 3000, 1))
    data = first.read_bytes()
    for i in range(1, 30):
        payload = b'fixture\0' + str(i).encode()
        chunk = struct.pack('!I', len(payload)) + b'tEXt' + payload + struct.pack('!I', zlib.crc32(b'tEXt' + payload))
        write_once(root / 'images30' / f'image-{i:05}.png', data[:-12] + chunk + data[-12:])
    manifest = {}
    for directory in sorted(root.iterdir()):
        if directory.is_dir():
            files = sorted(p for p in directory.iterdir() if p.is_file())
            manifest[directory.name] = {'files': len(files), 'bytes': sum(p.stat().st_size for p in files),
                                        'first_file': files[0].name, 'first_sha256': digest(files[0]),
                                        'first_bytes': files[0].stat().st_size,
                                        'all_content_sha256': hashlib.sha256(''.join(
                                            f'{p.name} {digest(p)}\n' for p in files).encode()).hexdigest()}
    return manifest


def process_stat(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return {'ppid': int(fields[1]), 'ticks': int(fields[11]) + int(fields[12])}


def idle_cpu(pid, seconds):
    # POSIX process CPU clocks include every thread and avoid /proc/stat's
    # hundredth-of-a-second quantization at low CPU utilization.
    clock_id = ctypes.c_int()
    error = ctypes.CDLL(None).clock_getcpuclockid(pid, ctypes.byref(clock_id))
    if error:
        raise OSError(error, 'clock_getcpuclockid failed')
    start = time.monotonic_ns()
    before = time.clock_gettime_ns(clock_id.value)
    time.sleep(seconds)
    elapsed_ns = time.monotonic_ns() - start
    cpu_ns = time.clock_gettime_ns(clock_id.value) - before
    return {'seconds': elapsed_ns / 1e9, 'cpu_ns': cpu_ns,
            'percent_one_core': cpu_ns / elapsed_ns * 100,
            'resolution_seconds': time.clock_getres(clock_id.value),
            'method': 'POSIX process CPU clock, all main-process threads, excludes helpers'}


def children(pid):
    relations = {}
    for proc in Path('/proc').iterdir():
        if proc.name.isdigit():
            try:
                relations[int(proc.name)] = process_stat(int(proc.name))['ppid']
            except (OSError, ValueError, ProcessLookupError):
                pass
    found = {pid}
    while True:
        more = {p for p, parent in relations.items() if parent in found}
        if more <= found:
            return sorted(found)
        found |= more


def memory(pid):
    values = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        key, value = line.split(':', 1)
        values[key] = int(value.split()[0]) * 1024
    return {'rss_bytes': values['Rss'], 'pss_bytes': values['Pss'],
            'private_bytes': values['Private_Clean'] + values['Private_Dirty'],
            'anonymous_bytes': values['Anonymous'], 'swap_bytes': values.get('Swap', 0)}


def memory_tree(pid):
    members = {}
    for child in children(pid):
        try:
            members[str(child)] = {**memory(child), 'executable': os.readlink(f'/proc/{child}/exe')}
        except (OSError, ProcessLookupError):
            pass
    if str(pid) not in members:
        raise RuntimeError('Main process disappeared during memory sample')
    return {'main': members[str(pid)], 'processes': members,
            'tree': {key: sum(item[key] for item in members.values()) for key in memory(pid)}}


def trace(path):
    events = []
    if not path.exists():
        return events
    for line in path.read_text().splitlines():
        fields = line.split()
        try:
            if fields[0] == 'key':
                events.append({'kind': 'key', 'ns': int(fields[1]), 'sequence': int(fields[2]), 'symbol': int(fields[3])})
            elif fields[0] in ('draw', 'frame'):
                events.append({'kind': fields[0], 'ns': int(fields[1]), 'sequence': int(fields[2]),
                               'busy': int(fields[3]), 'moving': int(fields[4]), 'selected': fields[5],
                               'size': list(map(int, fields[6:8]))})
        except (IndexError, ValueError):
            pass  # A writer may still be completing the last line.
    return events


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'samples': values, 'median': statistics.median(values),
            'p95': ordered[max(0, (95 * len(ordered) + 99) // 100 - 1)], 'max': max(values)}


def preview_metrics(sent, events, prefix, suffix, end_ns):
    """Keep input delivery, selection progress and settled latency distinct."""
    if not sent:
        raise ValueError('No input timestamps')
    received = [e for e in events if e['kind'] == 'key' and e['ns'] >= sent[0] and e['symbol'] == 65364]
    details = []
    previous = f'{prefix}-00000.{suffix}'
    for i, ns in enumerate(sent):
        deadline = sent[i + 1] if i + 1 < len(sent) else end_ns
        key = next((e for e in received if ns <= e['ns'] < deadline), None)
        expected = f'{prefix}-{i + 1:05}.{suffix}'
        frames = [e for e in events if key and e['kind'] == 'frame'
                  and ns <= e['ns'] < deadline and e['sequence'] == key['sequence']]
        loaded = next((e for e in frames if e['selected'] == expected and not e['busy']), None)
        match = next((e for e in frames if e['selected'] == expected
                      and not e['busy'] and not e['moving']), None)
        observed = frames[-1]['selected'] if frames else previous
        details.append({'expected': expected, 'observed': observed, 'advanced': observed != previous,
                        'sent_ns': ns, 'received_ns': key['ns'] if key else None,
                        'loaded_ns': loaded['ns'] if loaded else None,
                        'loaded_latency_ms': (loaded['ns'] - ns) / 1e6 if loaded else None,
                        'settled_ns': match['ns'] if match else None,
                        'latency_ms': (match['ns'] - ns) / 1e6 if match else None})
        previous = observed
    return {'transitions': details, 'sent': len(sent), 'received': len(received),
            'missed_delivery': sum(d['received_ns'] is None for d in details),
            'unadvanced_selection': sum(not d['advanced'] for d in details),
            'unsettled_or_wrong_selection': sum(d['settled_ns'] is None for d in details),
            'loaded_latency_ms': distribution([d['loaded_latency_ms'] for d in details if d['loaded_latency_ms'] is not None]),
            'latency_ms': distribution([d['latency_ms'] for d in details if d['latency_ms'] is not None])}


class Application:
    def __init__(self, suite, name, fixture, label, instrumented=True):
        self.suite, self.name = suite, name
        self.out = suite.out / label
        self.out.mkdir(parents=True, exist_ok=False)
        env = suite.env.copy()
        for kind in ('CONFIG', 'CACHE', 'STATE', 'DATA'):
            path = (suite.out / 'shared-cache' / name) if kind == 'CACHE' else self.out / kind.lower()
            path.mkdir(parents=True, exist_ok=True)
            env[f'XDG_{kind}_HOME'] = str(path)
        # Force equal logical dimensions through initial state and compositor tiling.
        state = self.out / 'state' / name
        state.mkdir()
        (state / 'state.toml').write_text('[window]\nwidth = 1200\nheight = 900\n')
        config = self.out / 'config.toml'
        config.write_text('[icon]\ntheme = "builtin"\nfollow_accent = false\n')
        self.trace_path = self.out / 'trace.txt'
        self.start_ns = time.monotonic_ns()
        env[name.upper() + '_STARTUP_NS'] = str(self.start_ns)
        env['PERFORMANCE_TRACE'] = str(self.trace_path)
        self.log_path = self.out / 'application.log'
        self.log = self.log_path.open('w')
        exe = suite.apps[name]['probe' if instrumented else 'executable']
        self.proc = subprocess.Popen([str(exe), '--config', str(config), str(suite.fixtures / fixture)],
                                     env=env, stdout=self.log, stderr=self.log)

    def settle(self, minimum_sequence=0, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f'{self.name} exited: {self.log_path.read_text()[-3000:]}')
            frames = [e for e in trace(self.trace_path) if e['kind'] == 'frame']
            if frames:
                last = frames[-1]
                if (last['sequence'] >= minimum_sequence and not last['busy'] and not last['moving']
                        and time.monotonic_ns() - last['ns'] > 200_000_000):
                    return last
            time.sleep(.02)
        raise RuntimeError(f'{self.name} did not settle: {self.trace_path}')

    def samples(self):
        result = []
        for _ in range(3):
            result.append(memory_tree(self.proc.pid))
            time.sleep(.1)
        return result

    def input(self, args, label):
        raw = command([self.suite.input, '1280', '960', *args], env=self.suite.env, timeout=90)
        (self.out / f'{label}-input.txt').write_text(raw)
        return raw

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()


class Suite:
    def __init__(self, args):
        self.args = args
        self.out = args.out.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.fixtures = args.fixtures.resolve()
        self.input = args.input.resolve()
        runtime = args.runtime.resolve()
        # Never inherit the desktop IPC socket or activation tokens.
        self.env = {k: v for k, v in os.environ.items() if k not in (
            'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_ACTIVATION_TOKEN', 'DESKTOP_STARTUP_ID',
            'LIBGL_ALWAYS_SOFTWARE', 'RUMMAGE_STARTUP_QUIT', 'NORDEND_STARTUP_QUIT')}
        self.env.update(XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=args.display,
                        UMBRIEL_SOCKET=str(runtime / f'umbriel-{args.display}.sock'),
                        DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={runtime}/no-system-bus')
        if runtime == Path(os.environ.get('XDG_RUNTIME_DIR', '/nonexistent')):
            raise RuntimeError('Refusing to inject input into the desktop runtime directory')
        outputs = json.loads(command(['umbriel', 'outputs', '--json'], env=self.env))
        if len(outputs) != 1 or outputs[0]['name'] != 'BENCH':
            raise RuntimeError('Expected one dedicated BENCH output; check compositor/socket isolation')
        mode = next(m for m in outputs[0]['modes'] if m['current'])
        if (mode['width'], mode['height']) != (1280, 960):
            raise RuntimeError('Benchmark output must be 1280x960')
        self.budget_ms = 1_000_000 / mode['refresh_mhz']
        self.apps = {}
        provenance = {}
        for name in ('rummage', 'nordend'):
            source = getattr(args, name + '_source').resolve()
            build = getattr(args, name + '_build').resolve()
            exe = build / name
            probe = getattr(args, name + '_probe').resolve()
            self.apps[name] = {'source': source, 'build': build, 'executable': exe, 'probe': probe}
            stripped = self.out / (name + '.stripped')
            subprocess.run(['strip', '-o', str(stripped), str(exe)], check=True)
            dynamic = command(['readelf', '-d', exe])
            provenance[name] = {
                'revision': command(['git', '-C', source, 'rev-parse', 'HEAD']).strip(),
                'source_diff': command(['git', '-C', source, 'diff']),
                'binary_sha256': digest(exe), 'instrumented_sha256': digest(probe),
                'binary_bytes': exe.stat().st_size, 'stripped_bytes': stripped.stat().st_size,
                'direct_libraries': re.findall(r'\(NEEDED\).*\[(.+?)\]', dynamic),
                'cmake_cache': (build / 'CMakeCache.txt').read_text(),
                'instrumentation': json.loads((probe.parent / 'instrumentation.json').read_text()),
            }
        self.report = {'schema': 1, 'invocation': sys.argv,
                       'harness_sha256': {p.name: digest(p) for p in Path(__file__).parent.glob('*.py')},
                       'machine': platform.uname()._asdict(), 'cpuinfo': Path('/proc/cpuinfo').read_text(),
                       'outputs': outputs, 'graphics_environment': {k: self.env.get(k) for k in ('__EGL_VENDOR_LIBRARY_FILENAMES', 'DRI_PRIME', 'LIBGL_ALWAYS_SOFTWARE')}, 'frame_budget_ms': self.budget_ms, 'apps': provenance,
                       'fixtures': fixtures(self.fixtures), 'startup': [], 'live': [], 'copy': [],
                       'cold_start': {'status': 'not measured' if not args.cold_cache_hook else 'hook enabled',
                                      'reason': 'True cold startup needs an explicitly supplied cache-reset hook on a disposable test host.'},
                       'method': {'memory': 'Live window /proc smaps_rollup; bytes; main RSS/PSS/private/anonymous and helper tree separately. Excludes compositor and unaccounted GPU allocations.',
                                  'cache': 'Warm page cache and shared per-application shader/thumbnail cache; fresh state/config per process.',
                                  'frames': 'Wayland frame callbacks on a headless output, not physical presentation or input-to-photon latency.',
                                  'preview': 'One discarded warm-up process per fixture/app before measured rounds. 28 Down transitions, 400 ms apart; first callback for expected selection with loading=false and animating=false. Includes key delivery.',
                                  'cold_cache_hook': args.cold_cache_hook,
                                  'copy': '1,000,000,000 allocated pseudorandom bytes; app transfer engine vs cp, including destination file and directory fsync. Hash verified outside timed interval; native copy_file_range behavior retained.'}}
        self.keyboard = subprocess.Popen([str(self.input), '1', '1', 'keyboard-only', 'mod', 'none', 'mark', 'ready', 'pause', '86400000'],
                                         env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if self.keyboard.stdout.readline().strip() != 'ready':
            raise RuntimeError('Persistent virtual keyboard failed')
        self.save()

    def save(self):
        (self.out / 'results.json').write_text(json.dumps(self.report, indent=2) + '\n')

    def startup(self):
        # Alternate execution order to reduce drift. Keep independent startup caches
        # per app/fixture; warm both applications before taking samples.
        for fixture in ('files20', 'files10000'):
            for round_no in range(self.args.startup_runs + 1):
                for name in (('rummage', 'nordend') if round_no % 2 == 0 else ('nordend', 'rummage')):
                    app = Application(self, name, fixture, f'start-{fixture}-{round_no}-{name}', instrumented=False)
                    try:
                        # Use the shipped startup marker, not instrumentation, for timing.
                        deadline = time.monotonic() + 20
                        while 'startup: folder-shown ' not in app.log_path.read_text():
                            if app.proc.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError(f'Missing startup marker: {app.log_path}')
                            time.sleep(.005)
                        raw = app.log_path.read_text()
                        steps = {k: float(v) for k, v in re.findall(r'startup: (\S+) ([\d.]+) ms', raw)}
                        self.report['startup'].append({'app': name, 'fixture': fixture, 'warmup': round_no == 0,
                                                       'round': round_no, 'steps_ms': steps})
                    finally:
                        app.close()
            self.save()
        if self.args.cold_cache_hook:
            # Deliberately no built-in global drop_caches command. The caller owns
            # the disposable host and supplies its explicit reset procedure.
            for round_no in range(self.args.startup_runs):
                for name in (('rummage', 'nordend') if round_no % 2 == 0 else ('nordend', 'rummage')):
                    subprocess.run(self.args.cold_cache_hook, check=True, shell=True)
                    app = Application(self, name, 'files20', f'cold-{round_no}-{name}', instrumented=False)
                    try:
                        deadline = time.monotonic() + 30
                        while 'startup: folder-shown ' not in app.log_path.read_text():
                            if app.proc.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError('Missing cold startup marker')
                            time.sleep(.005)
                        steps = {k: float(v) for k, v in re.findall(r'startup: (\S+) ([\d.]+) ms', app.log_path.read_text())}
                        self.report['startup'].append({'app': name, 'fixture': 'files20', 'cold': True,
                                                       'warmup': False, 'round': round_no, 'steps_ms': steps})
                    finally:
                        app.close()
            self.save()

    def live(self):
        scenarios = self.args.live_fixtures
        for round_no in range(-1, self.args.runs):
            for fixture in scenarios:
                for name in (('rummage', 'nordend') if round_no % 2 == 0 else ('nordend', 'rummage')):
                    print(f'live {round_no + 1}/{self.args.runs} {fixture} {name}', flush=True)
                    app = Application(self, name, fixture, f'live-{round_no}-{fixture}-{name}')
                    result = {'app': name, 'fixture': fixture, 'round': round_no, 'directory': str(app.out)}
                    try:
                        app.settle()
                        if self.args.expect_renderer and self.args.expect_renderer not in app.log_path.read_text():
                            raise RuntimeError(f'Unexpected graphics backend: {app.log_path}')
                        if round_no < 0:
                            if fixture in ('images30', 'c-small', 'c-large'):
                                app.input(['keyboard-only', 'tap', '108'], 'warm-selection')
                                app.settle(1)
                                app.input(['keyboard-only', 'tap', '57'], 'warm-quicklook')
                                app.settle(2)
                            continue
                        time.sleep(1)
                        result['idle_memory'] = app.samples()
                        maps = Path(f'/proc/{app.proc.pid}/maps').read_text()
                        result['loaded_libraries'] = sorted({line.split()[-1] for line in maps.splitlines()
                                                              if '/' in line and re.search(r'\.so(?:\.|$)', line.split()[-1])})
                        result['compositor_windows'] = json.loads(command(['umbriel', 'windows', '--json'], env=self.env))
                        result['draw_sizes'] = sorted({tuple(e['size']) for e in trace(app.trace_path) if e['kind'] == 'draw'})
                        result['renderer_log'] = [line for line in app.log_path.read_text().splitlines() if re.search(r'GL |EGL|renderer', line)]
                        if fixture == 'files20':
                            result['idle_cpu'] = idle_cpu(app.proc.pid, self.args.idle_seconds)
                        if fixture == 'files10000':
                            args = ['move', '450', '350']
                            for _ in range(int(self.args.scroll_seconds * 125)):
                                args += ['axis', 'vertical', '12', 'pause', '8']
                            args += ['axis-stop', 'vertical']
                            raw = app.input(args, 'scroll')
                            axes = [int(m) for m in re.findall(r'^axis (\d+)$', raw, re.M)]
                            app.settle()
                            frames = [e['ns'] for e in trace(app.trace_path) if e['kind'] == 'frame'
                                      and axes[0] + 500_000_000 <= e['ns'] <= axes[-1] - 500_000_000]
                            gaps = [(b - a) / 1e6 for a, b in zip(frames, frames[1:])]
                            if len(gaps) < 20:
                                raise RuntimeError('Too few scrolling callbacks; check pointer coordinates')
                            result['scroll'] = {'gaps_ms': distribution(gaps), 'late_frames': sum(g > 1.5 * self.budget_ms for g in gaps),
                                                'gap_count': len(gaps), 'input_events': len(axes),
                                                'input_duration_s': (axes[-1] - axes[0]) / 1e9}
                            result['after_scroll_memory'] = app.samples()
                        if fixture in ('images30', 'c-small', 'c-large'):
                            # Prime the first selection outside the timed transitions.
                            app.input(['keyboard-only', 'tap', '108'], 'prime')
                            app.settle(1)
                            result['first_preview_memory'] = app.samples()
                            prefix, suffix = ('image', 'png') if fixture == 'images30' else ('sample', 'c')
                            args = ['keyboard-only']
                            for _ in range(28):
                                args += ['tap', '108', 'pause', '400']
                            raw = app.input(args, 'preview')
                            app.settle(29)
                            sent = [int(m) for m in re.findall(r'^send (\d+) 108$', raw, re.M)]
                            events = trace(app.trace_path)
                            result['preview'] = preview_metrics(sent, events, prefix, suffix, time.monotonic_ns())
                            result['after_preview_memory'] = app.samples()
                            app.input(['keyboard-only', 'tap', '57'], 'quicklook')
                            app.settle(30)
                            result['quicklook_memory'] = app.samples()
                        self.report['live'].append(result)
                        self.save()
                    finally:
                        app.close()

    def idle(self):
        self.report['idle_cpu_precise'] = []
        for round_no in range(self.args.runs):
            for name in (('rummage', 'nordend') if round_no % 2 == 0 else ('nordend', 'rummage')):
                print(f'idle CPU {round_no + 1}/{self.args.runs} {name}', flush=True)
                app = Application(self, name, 'files20', f'idle-{round_no}-{name}')
                try:
                    app.settle()
                    time.sleep(1)
                    if self.args.expect_renderer and self.args.expect_renderer not in app.log_path.read_text():
                        raise RuntimeError(f'Unexpected graphics backend: {app.log_path}')
                    self.report['idle_cpu_precise'].append({'app': name, 'round': round_no,
                        'measurement': idle_cpu(app.proc.pid, self.args.idle_seconds)})
                    self.save()
                finally:
                    app.close()

    def copy(self):
        root = self.args.copy_root.resolve()
        root.mkdir(parents=True, exist_ok=True)
        source = root / 'source'
        source.mkdir(exist_ok=True)
        file = source / 'payload.bin'
        if not file.exists():
            # SHAKE provides deterministic, incompressible-looking data in 1 MB blocks.
            with file.open('wb') as stream:
                for i in range(1000):
                    stream.write(hashlib.shake_256(f'rummage-copy-fixture-{i}'.encode()).digest(1_000_000))
                stream.flush()
                os.fsync(stream.fileno())
        if file.stat().st_size != 1_000_000_000 or file.stat().st_blocks * 512 < file.stat().st_size:
            raise RuntimeError('Copy fixture must contain 1 GB of allocated data')
        expected = digest(file)
        self.report['copy_fixture'] = {'bytes': file.stat().st_size, 'allocated_bytes': file.stat().st_blocks * 512,
                                       'sha256': expected, 'filesystem': command(['stat', '-f', '-c', '%T', root]).strip()}
        for round_no in range(self.args.runs):
            names = ('rummage', 'nordend', 'cp') if round_no % 2 == 0 else ('cp', 'nordend', 'rummage')
            for name in names:
                # Equal warm source-cache conditions before every timed copy.
                with file.open('rb') as stream:
                    while stream.read(8 * 1024 * 1024):
                        pass
                dest = root / f'{self.out.name}-{round_no}-{name}'
                dest.mkdir(exist_ok=False)
                args = (['cp', '--reflink=never', '--sparse=never', file, dest / file.name] if name == 'cp'
                        else [self.apps[name]['build'] / (name + '-transfer'), source, dest, file.name])
                start = time.monotonic_ns()
                output = command(args, stderr=subprocess.STDOUT)
                with (dest / file.name).open('rb') as copied:
                    os.fsync(copied.fileno())
                fd = os.open(dest, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(fd)
                finally:
                    os.close(fd)
                elapsed = (time.monotonic_ns() - start) / 1e9
                actual = digest(dest / file.name)
                if actual != expected:
                    raise RuntimeError('Copy integrity check failed')
                self.report['copy'].append({'app': name, 'round': round_no, 'seconds_including_sync': elapsed,
                                            'sha256': actual, 'allocated_bytes': (dest / file.name).stat().st_blocks * 512,
                                            'stdout': output})
                (dest / file.name).unlink()
                dest.rmdir()
                self.save()

    def close(self):
        self.keyboard.terminate()
        self.keyboard.wait(timeout=5)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('rummage', 'nordend'):
        for kind in ('source', 'build', 'probe'):
            p.add_argument(f'--{name}-{kind}', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--display', default='wayland-0')
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--copy-root', type=Path, required=True, help='Disk-backed directory, not tmpfs')
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--startup-runs', type=int, default=9)
    p.add_argument('--idle-seconds', type=float, default=10)
    p.add_argument('--scroll-seconds', type=float, default=15)
    p.add_argument('--expect-renderer', help='Fail if the application GL log does not contain this renderer name')
    p.add_argument('--live-fixtures', nargs='+', choices=('files20', 'files10000', 'images30', 'c-small', 'c-large'),
                   default=['files20', 'files10000', 'images30', 'c-small', 'c-large'])
    p.add_argument('--cold-cache-hook', help='Explicit shell command on a disposable test host; never enabled by default')
    p.add_argument('--phases', nargs='+', choices=('startup', 'live', 'copy', 'idle'), default=['startup', 'live', 'copy'])
    a = p.parse_args()
    if a.runs < 1 or a.startup_runs < 1 or a.scroll_seconds < 2 or a.idle_seconds <= 0:
        p.error('Positive run counts, idle duration and at least two seconds of scrolling are required')
    suite = Suite(a)
    try:
        for phase in a.phases:
            print(f'phase: {phase}', flush=True)
            getattr(suite, phase)()
    finally:
        suite.close()


if __name__ == '__main__':
    main()
