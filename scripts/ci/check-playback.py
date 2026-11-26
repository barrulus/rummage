#!/usr/bin/env python3
"""Exercise the installed, sandboxed decoder protocol with synthetic media (no audio device)."""
import array
import ctypes
import fcntl
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import wave

MAGIC = 0x52504C31
HEADER = struct.Struct('=IIqIIII')


def installed_codecs(helper):
    # Load the exact library linked by this installed helper, including its RPATH.
    # Never turn a discovery failure into a skipped decoder test.
    linked = subprocess.run(['ldd', str(helper)], check=True, capture_output=True,
                            text=True, timeout=8).stdout
    matches = re.findall(r'^\s*libavcodec\.so\S*\s+=>\s+(/\S+)\s+\(', linked, re.MULTILINE)
    if len(matches) != 1:
        raise RuntimeError(f'Cannot identify playback libavcodec from ldd:\n{linked}')
    library = ctypes.CDLL(matches[0])
    # AVCodecDescriptor starts with enum AVCodecID. Access only that stable first field.
    library.avcodec_descriptor_get_by_name.argtypes = [ctypes.c_char_p]
    library.avcodec_descriptor_get_by_name.restype = ctypes.POINTER(ctypes.c_int)
    library.avcodec_find_decoder.argtypes = [ctypes.c_int]
    library.avcodec_find_decoder.restype = ctypes.c_void_p
    result = {}
    for name in ['h264', 'hevc', 'vp9', 'av1', 'aac']:
        descriptor = library.avcodec_descriptor_get_by_name(name.encode('ascii'))
        if not descriptor:
            raise RuntimeError(f'Unknown fixture codec {name} in {matches[0]}')
        result[name] = bool(library.avcodec_find_decoder(descriptor[0]))
    print(f'playback codec discovery: {matches[0]}: {result}', flush=True)
    return result


class Decoder:
    def __init__(self, helper, path):
        self.socket, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.path = path
        self.log = tempfile.TemporaryFile(mode='w+')
        def child_socket():
            os.dup2(child.fileno(), 3)
            # The parent end must not survive exec: it would prevent EOF on close().
            for fd in (self.socket.fileno(), child.fileno()):
                if fd != 3:
                    os.close(fd)
        # Duplicate in the child, retaining both socket descriptors through subprocess's close.
        # Keep fd 3 explicitly even when another open descriptor occupied it before socketpair.
        self.process = subprocess.Popen([str(helper)],
            pass_fds=tuple({3, self.socket.fileno(), child.fileno()}), stderr=self.log,
            preexec_fn=child_socket)
        child.close()
        self.socket.settimeout(8)
        self.file = os.open(path, os.O_RDONLY)
        self.first = True

    def step(self, position=0, seek=False):
        descriptors = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [self.file]))] if self.first else []
        try:
            self.socket.sendmsg([struct.pack('=IIq', MAGIC, int(seek), position)], descriptors)
            self.first = False
            data, ancillary, flags, _ = self.socket.recvmsg(1000, socket.CMSG_SPACE(8))
            assert not flags and len(data) == HEADER.size, (flags, data)
        except (OSError, AssertionError) as error:
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
            self.log.seek(0)
            raise RuntimeError(f'{self.path}: protocol failure {error}; '
                               f'helper exit {self.process.returncode}; stderr:\n{self.log.read()}') from error
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
        try:
            self.process.wait(timeout=8)
            self.log.seek(0)
            assert self.process.returncode == 0, (self.path, self.process.returncode, self.log.read())
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=3)
            self.log.close()


def check_video(helper, path, codecs, available):
    decoder = Decoder(helper, path)
    try:
        result = decoder.step()
        missing = [name for name in codecs if not available[name]]
        if missing:
            assert result == (2, 0, 0, 0, 0, 0, b''), (path.name, missing, result[:6])
            print(f'playback: {path.name}: unavailable {missing}; bounded unsupported reply', flush=True)
            return
        assert result[0] < 2, (path.name, result[:6])
        assert result[2:4] == ((48, 96) if path.name == 'rotated.mp4' else (96, 48))
        if path.name == 'metadata.mp4':
            assert result[5] == 1 and result[4] > 0
        assert decoder.step(100, True)[0] < 2, path.name
        if path.name == 'fade.mp4':
            assert result[6][0] < 12  # buffered future frames cannot replace time zero
            assert decoder.step(2000, True)[6][0] > 220
            assert decoder.step(0, True)[6][0] < 12
    finally:
        decoder.close()  # An unavailable codec must also exit cleanly, never crash or hang.


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
        available = installed_codecs(helper)
        for name, codecs in [('red.mp4', ['h264']), ('red-hevc.mp4', ['hevc']),
                             ('red-vp9.webm', ['vp9']), ('red-av1.mkv', ['av1']),
                             ('metadata.mp4', ['h264', 'aac']), ('rotated.mp4', ['h264']),
                             ('fade.mp4', ['h264'])]:
            check_video(helper, Path(fixtures) / name, codecs, available)
    print('playback: sandbox PCM, EOF, seek, damaged/external input and optional AV fixtures passed')

if __name__ == '__main__':
    check(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else None)
