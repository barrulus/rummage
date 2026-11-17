#!/usr/bin/env python3
"""Run a command on a private hardware-rendered Umbriel headless display.

The child command receives {runtime} placeholders as arguments. Its inherited
XDG_RUNTIME_DIR is unchanged so run.py can check that the display is isolated.
Compositor autostart, Xwayland and desktop session/system buses are disabled.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--directory', type=Path, required=True)
    p.add_argument('--render-device', required=True, help='Explicit /dev/dri/renderD* device')
    p.add_argument('--egl-vendor', type=Path, help='Optional explicit GLVND vendor JSON, also used by clients')
    p.add_argument('command', nargs=argparse.REMAINDER)
    a = p.parse_args()
    root = a.directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    runtime = root / 'runtime'
    runtime.mkdir(mode=0o700)
    config = root / 'compositor.toml'
    config.write_text('[general]\nxwayland = false\nshow_cheatsheet = false\nautostart = []\n')
    env = {k: v for k, v in os.environ.items() if k not in (
        'WAYLAND_DISPLAY', 'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'UMBRIEL_SOCKET',
        'WLR_DRM_DEVICES', 'LIBGL_ALWAYS_SOFTWARE', 'XDG_ACTIVATION_TOKEN', 'DESKTOP_STARTUP_ID')}
    env.update(XDG_RUNTIME_DIR=str(runtime), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1',
               WLR_LIBINPUT_NO_DEVICES='1', UMBRIEL_CONFIG_WATCH='0',
               WLR_RENDER_DRM_DEVICE=a.render_device,
               DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={runtime}/no-system-bus')
    client_env = os.environ.copy()
    if a.egl_vendor:
        env['__EGL_VENDOR_LIBRARY_FILENAMES'] = str(a.egl_vendor)
        client_env['__EGL_VENDOR_LIBRARY_FILENAMES'] = str(a.egl_vendor)
    with (root / 'compositor.log').open('w') as log:
        proc = subprocess.Popen(['umbriel', '-c', str(config)], env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            socket = runtime / 'umbriel-wayland-0.sock'
            deadline = time.monotonic() + 20
            while not socket.exists():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f'Compositor failed; see {root / "compositor.log"}')
                time.sleep(.05)
            ipc_env = dict(env, UMBRIEL_SOCKET=str(socket), WAYLAND_DISPLAY='wayland-0')
            def ipc(*args):
                return subprocess.check_output(['umbriel', *args], env=ipc_env, text=True)
            initial = json.loads(ipc('outputs', '--json'))
            ipc('output-create', 'BENCH', '1280x960@120')
            for output in initial:
                ipc('output-destroy', output['name'])
            (root / 'outputs.json').write_text(ipc('outputs', '--json'))
            (root / 'environment.json').write_text(json.dumps({k: v for k, v in env.items() if k.startswith(('WLR_', 'UMBRIEL_', '__EGL_'))}, indent=2))
            args = a.command[1:] if a.command[:1] == ['--'] else a.command
            if not args:
                p.error('Supply a benchmark command after --')
            subprocess.run([arg.replace('{runtime}', str(runtime)) for arg in args], env=client_env, check=True)
        finally:
            # Only this compositor's newly created process group is terminated.
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()


if __name__ == '__main__':
    main()
