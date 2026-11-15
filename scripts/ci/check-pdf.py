#!/usr/bin/env python3
"""Render fixtures through the installed PDF helper and its real sandbox."""
import array
import os
from pathlib import Path
import signal
import socket
import struct
import sys

helper = sys.argv[1]
fixtures = Path(sys.argv[2])


def render(name, size=128, expected_failure=False):
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
        with (fixtures / name).open('rb') as source:
            parent.sendmsg([struct.pack('=IIII', 0x4E505251, 0, size, size)],
                           [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [source.fileno()]))])
        data, controls, flags, _ = parent.recvmsg(8192, socket.CMSG_SPACE(8))
        for level, kind, payload in controls:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                descriptors.frombytes(payload)
        assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC), name
        assert len(data) >= 24, (name, data)
        magic, failure, _, _, width, height, _, _ = struct.unpack('=IBBHIIII', data[:24])
        assert magic == 0x4E505253, (name, magic)
        if expected_failure:
            assert failure != 0 and not descriptors, (name, failure)
            return
        assert failure == 0 and len(descriptors) == 1, (name, failure, data[24:])
        pixels = os.pread(descriptors[0], width * height * 4, 0)
        assert len(pixels) == width * height * 4, name
        return width, height, pixels
    finally:
        for descriptor in descriptors:
            os.close(descriptor)
        parent.close()
        # Bound cleanup even when a broken helper ignores a closed socket.
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)


width, height, pixels = render('red.pdf')
assert (width, height) == (128, 64)
assert pixels[0] > 220 and pixels[1] < 30 and pixels[3] == 255
assert render('rotated.pdf')[:2] == (64, 128)
render('locked.pdf', expected_failure=True)
assert render('owner-only.pdf')[:2] == (128, 64)
_, _, pixels = render('text.pdf', 200)
assert pixels[0] == 255 and sum(value < 100 for value in pixels[::4]) > 200
for name in ('form.pdf', 'form-rotated.pdf'):
    width, height, pixels = render(name, 200)
    assert width * height == 200 * 120
    colors = list(zip(pixels[0::4], pixels[1::4], pixels[2::4]))
    assert sum(r > 220 and g < 30 and b < 30 for r, g, b in colors) > 4000
    assert sum(r < 30 and g < 30 and b < 30 for r, g, b in colors) > 200
    assert sum(r < 30 and g < 30 and b > 220 for r, g, b in colors) > 300
print('Installed PDF helper: first page, rotation, fonts and forms passed under the sandbox.')
