#!/usr/bin/env python3
"""Exercise the installed, sandboxed decoder protocol with synthetic media (no audio device)."""
import array
import fcntl
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import wave

MAGIC = 0x52504C31
HEADER = struct.Struct('=IIqIIII')

class Decoder:
    def __init__(self, helper, path):
        self.socket, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        # Duplicate in the child, retaining both socket descriptors through subprocess's close.
        self.process = subprocess.Popen([str(helper)], pass_fds=(self.socket.fileno(), child.fileno()),
            preexec_fn=lambda: os.dup2(child.fileno(), 3))
        child.close()
        self.socket.settimeout(8)
        self.file = os.open(path, os.O_RDONLY)
        self.first = True

    def step(self, position=0, seek=False):
        descriptors = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [self.file]))] if self.first else []
        self.socket.sendmsg([struct.pack('=IIq', MAGIC, int(seek), position)], descriptors)
        self.first = False
        data, ancillary, flags, _ = self.socket.recvmsg(1000, socket.CMSG_SPACE(8))
        assert not flags and len(data) == HEADER.size, (flags, data)
        magic, status, duration, width, height, samples, audio = HEADER.unpack(data)
        assert magic == MAGIC and status <= 3 and 0 <= duration <= 604800000
        assert width <= 1280 and height <= 1280 and samples <= 96000 and samples % 2 == 0
        received = [fd for level, kind, payload in ancillary
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
                    for fd in array.array('i', payload)]
        body = b''
        try:
            if status < 2:
                assert len(received) == 1
                fd = received[0]
                size = width * height * 4 + samples * 2
                assert os.fstat(fd).st_size == size
                assert fcntl.fcntl(fd, fcntl.F_GET_SEALS) & (fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_WRITE) == (fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_WRITE)
                body = os.pread(fd, size, 0)
                assert len(body) == size
            else:
                assert not received
        finally:
            for fd in received:
                os.close(fd)
        return status, duration, width, height, samples, audio, body

    def close(self):
        os.close(self.file)
        self.socket.close()
        self.process.wait(timeout=8)
        assert self.process.returncode == 0, self.process.returncode


def check(helper, fixtures=None):
    with tempfile.TemporaryDirectory(prefix='rummage-playback-') as temporary:
        path = Path(temporary) / 'tone.wav'
        with wave.open(str(path), 'wb') as output:
            output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            # A nonzero signal verifies exact PCM transport, not merely silent buffers.
            output.writeframes(struct.pack('<hh', 1000, -1000) * 48000)
        decoder = Decoder(helper, path)
        samples = 0
        for position in range(0, 1200, 50):
            result = decoder.step(position)
            status, duration, width, height, count, audio, body = result
            assert (duration, width, height, audio) == (1000, 0, 0, 1)
            assert not body or body == struct.pack('<hh', 1000, -1000) * (count // 2)
            samples += count
            if status == 1:
                break
        assert status == 1 and samples == 96000, samples
        assert decoder.step(400, True)[0] == 0
        assert decoder.step(0, True)[4] > 0
        decoder.close()
        for name, body in [('broken.mp4', b'not a video'), ('remote.m3u8', b'#EXTM3U\nhttp://127.0.0.1/private.ts\n')]:
            broken = Path(temporary) / name
            broken.write_bytes(body)
            decoder = Decoder(helper, broken)
            assert decoder.step()[0] == 2
            decoder.close()
    if fixtures:
        for name in ['red.mp4', 'red-hevc.mp4', 'red-vp9.webm', 'red-av1.mkv', 'metadata.mp4', 'rotated.mp4']:
            decoder = Decoder(helper, Path(fixtures) / name)
            result = decoder.step()
            assert result[0] < 2, (name, result[:6])
            assert result[2:4] == ((48, 96) if name == 'rotated.mp4' else (96, 48))
            if name == 'metadata.mp4':
                assert result[5] == 1 and result[4] > 0
            decoder.step(100, True)
            decoder.close()
        decoder = Decoder(helper, Path(fixtures) / 'fade.mp4')
        assert decoder.step()[6][0] < 12  # buffered future frames cannot replace time zero
        assert decoder.step(2000, True)[6][0] > 220
        assert decoder.step(0, True)[6][0] < 12
        decoder.close()
    print('playback: sandbox PCM, EOF, seek, damaged/external input and optional AV fixtures passed')

if __name__ == '__main__':
    check(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else None)
