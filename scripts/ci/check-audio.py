#!/usr/bin/env python3
"""Check fragmented PCM delivery against a private PulseAudio null sink; never use host audio."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time


def check(helper, pulse_bin):
    with tempfile.TemporaryDirectory(prefix='rummage-audio-test-') as temporary:
        env = os.environ.copy()
        env.update(DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent", PULSE_RUNTIME_PATH=temporary, PULSE_STATE_PATH=temporary,
                   PULSE_SERVER='unix:' + temporary + '/native')
        config = Path(temporary) / 'default.pa'
        config.write_text('load-module module-native-protocol-unix socket=' + temporary +
                          '/native auth-anonymous=1\nload-module module-null-sink '
                          'sink_name=rummage_test\nset-default-sink rummage_test\n')
        with tempfile.TemporaryFile(mode='w+') as log:
            server = subprocess.Popen([str(pulse_bin / 'pulseaudio'), '--daemonize=no',
                '--exit-idle-time=-1', '--log-level=warning', '--file=' + str(config),
                '--disallow-module-loading'], env=env, stdout=log, stderr=log)
            sink = None
            try:
                for _ in range(100):
                    if Path(temporary, 'native').exists():
                        break
                    if server.poll() is not None:
                        log.seek(0)
                        raise RuntimeError('Private sound server exited: ' + log.read())
                    time.sleep(.02)
                sink = subprocess.Popen([str(helper)], stdin=subprocess.PIPE, env=env)
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
                    env=env, capture_output=True, text=True, check=True)
                assert 's16le 2ch 48000Hz' in streams.stdout, streams.stdout
                sink.stdin.close()
                assert sink.wait(timeout=3) == 0
            finally:
                if sink is not None and sink.poll() is None:
                    sink.kill()
                    sink.wait(timeout=5)
                server.terminate()
                server.wait(timeout=5)
    print('audio: fragmented PCM accepted by private stereo 48 kHz null sink, bounded EOF completion')


if __name__ == '__main__':
    check(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
