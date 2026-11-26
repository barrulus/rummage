#!/usr/bin/env python3
"""Check fragmented PCM delivery against a private PulseAudio null sink; never use host audio."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time


def wait_ready(server, pulse_bin, env):
    deadline = time.monotonic() + 3
    detail = 'no response'
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError(f'Private sound server exited with status {server.returncode}')
        try:
            response = subprocess.run([str(pulse_bin / 'pactl'), 'get-default-sink'],
                                      env=env, capture_output=True, text=True, timeout=.25)
            if response.returncode == 0 and response.stdout.strip() == 'rummage_test':
                return
            detail = (response.stderr or response.stdout).strip()
        except subprocess.TimeoutExpired:
            detail = 'pactl timed out'
        time.sleep(.02)
    raise RuntimeError('Private null sink was not ready: ' + detail)


def check(helper, pulse_bin):
    with tempfile.TemporaryDirectory(prefix='rummage-audio-test-') as temporary:
        # Even anonymous native-protocol servers initialize a cookie. Nix builders have an
        # unwritable HOME, so both server and clients must use this private fixture cookie.
        cookie = Path(temporary) / 'cookie'
        cookie.write_bytes(os.urandom(256))
        cookie.chmod(0o600)
        env = os.environ.copy()
        env.update(DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent',
                   PULSE_RUNTIME_PATH=temporary, PULSE_STATE_PATH=temporary,
                   PULSE_SERVER='unix:' + temporary + '/native', PULSE_COOKIE=str(cookie),
                   PULSE_SINK='rummage_test', PULSE_LOG='4')
        config = Path(temporary) / 'default.pa'
        config.write_text('.fail\nload-module module-native-protocol-unix socket=' + temporary +
                          '/native auth-cookie=' + str(cookie) + '\nload-module module-null-sink '
                          'sink_name=rummage_test\nset-default-sink rummage_test\n')
        with tempfile.TemporaryFile(mode='w+') as log, tempfile.TemporaryFile(mode='w+') as audio_log:
            server = subprocess.Popen([str(pulse_bin / 'pulseaudio'), '--daemonize=no',
                '--exit-idle-time=-1', '--log-level=warning', '--file=' + str(config),
                '--disallow-module-loading'], env=env, stdout=log, stderr=log)
            sink = None
            try:
                # A socket alone does not prove that the native protocol and default sink
                # finished loading. Require an authenticated round trip before sending PCM.
                wait_ready(server, pulse_bin, env)
                sink = subprocess.Popen([str(helper)], stdin=subprocess.PIPE, env=env,
                                        stderr=audio_log)
                pcm = struct.pack('<hh', 1000, -1000) * 24000
                offset = 0
                for count in [1, 2, 3, 5, 7, 13, 17]:
                    sink.stdin.write(pcm[offset:offset + count])
                    sink.stdin.flush()
                    offset += count
                    time.sleep(.02)
                    assert sink.poll() is None, 'Rejected a fragmented stereo PCM frame'
                sink.stdin.write(pcm[offset:])
                sink.stdin.flush()
                streams = subprocess.run([str(pulse_bin / 'pactl'), 'list', 'sink-inputs', 'short'],
                    env=env, capture_output=True, text=True, check=True, timeout=3)
                assert 's16le 2ch 48000Hz' in streams.stdout, streams.stdout
                sink.stdin.close()
                assert sink.wait(timeout=3) == 0
            except Exception as error:
                log.seek(0)
                audio_log.seek(0)
                raise RuntimeError(f'{error}\nAudio helper stderr:\n{audio_log.read()}'
                                   f'\nPrivate PulseAudio stderr:\n{log.read()}') from error
            finally:
                if sink is not None and sink.poll() is None:
                    sink.kill()
                    sink.wait(timeout=5)
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)
    print('audio: fragmented PCM accepted by private stereo 48 kHz null sink, bounded EOF completion')


if __name__ == '__main__':
    check(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
