#!/usr/bin/env python3
"""Exercise the installed font helper and its real sandbox (protocol 8)."""
import array
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import tempfile

helper, font = sys.argv[1:]


def render(path, size=128, expected_failure=False):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        parent.close()
        os.dup2(child.fileno(), 3, inheritable=True)
        os.closerange(4, 1024)
        os.execl(helper, helper)
    child.close()
    parent.settimeout(30)
    descriptors = array.array('i')
    try:
        with Path(path).open('rb') as source:
            parent.sendmsg([struct.pack('=IIIII', 0x4E505251, 0, size, size, 0)],
                           [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [source.fileno()]))])
        data, controls, flags, _ = parent.recvmsg(8192, socket.CMSG_SPACE(8))
        for level, kind, payload in controls:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                descriptors.frombytes(payload)
        assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
        assert len(data) >= 28, data
        magic, failure, _, _, width, height, _, _, pages = struct.unpack('=IBBHIIIII', data[:28])
        assert magic == 0x4E505253
        if expected_failure:
            assert failure != 0 and not descriptors
            return
        assert failure == 0 and len(descriptors) == 1, (failure, data[28:])
        assert pages == 0 and width == min(size, 1024) and 0 < height <= 1024
        pixels = os.pread(descriptors[0], width * height * 4, 0)
        assert len(pixels) == width * height * 4
        assert sum(value < 128 for value in pixels[::4]) > 50
        assert all(value == 255 for value in pixels[3::4])
    finally:
        for descriptor in descriptors:
            os.close(descriptor)
        parent.close()
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)


render(font)
render(font, 256)
render(font, 8000)
with tempfile.TemporaryDirectory() as directory:
    bad = Path(directory) / 'bad.ttf'
    bad.write_bytes(b'not a font')
    render(bad, expected_failure=True)
    with bad.open('wb') as out:
        out.truncate(32 * 1024 * 1024 + 1)
    render(bad, expected_failure=True)
print('Installed font helper: specimen, scales, damaged and oversized inputs passed under the sandbox.')
