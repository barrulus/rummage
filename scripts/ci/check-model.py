#!/usr/bin/env python3
"""Exercise the installed model helper and its real sandbox (protocol 8)."""
import array
import base64
import json
import zlib
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import tempfile

helper = sys.argv[1]


def render(path, size=128, expected_failure=False, allow_rejection=False):
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
        response = (Path(path).name, failure, width, height, len(descriptors), data[28:])
        if expected_failure:
            assert failure != 0 and not descriptors, response
            return False
        if allow_rejection and failure != 0:
            assert not descriptors, response
            return False
        assert failure == 0 and len(descriptors) == 1, response
        assert pages == 0 and width == min(size, 1024) and 0 < height <= 1024
        pixels = os.pread(descriptors[0], width * height * 4, 0)
        assert len(pixels) == width * height * 4
        assert sum(value < 128 for value in pixels[::4]) > 50
        assert all(value == 255 for value in pixels[3::4])
        if os.getenv("RUMMAGE_MODEL_PNG"):
            def chunk(kind, content):
                return struct.pack('>I', len(content)) + kind + content + struct.pack('>I', zlib.crc32(kind + content))
            raw = b''.join(b'\0' + pixels[y*width*4:(y+1)*width*4] for y in range(height))
            Path(os.environ["RUMMAGE_MODEL_PNG"]).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
        return True
    finally:
        for descriptor in descriptors:
            os.close(descriptor)
        parent.close()
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    vertices = [(0., 0., 0.), (1., 0., 0.), (0., 1., 0.), (0., 0., 1.)]
    faces = [(0, 1, 2), (0, 3, 1), (0, 2, 3), (1, 3, 2)]
    obj = ''.join('v %s %s %s\n' % v for v in vertices) + ''.join('f %s %s %s\n' % tuple(i+1 for i in f) for f in faces)
    ply = 'ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nelement face 4\nproperty list uchar int vertex_indices\nend_header\n' + ''.join('%s %s %s\n' % v for v in vertices) + ''.join('3 %s %s %s\n' % f for f in faces)
    stl = 'solid tetra\n' + ''.join('facet normal 0 0 0\nouter loop\n' + ''.join('vertex %s %s %s\n' % vertices[i] for i in f) + 'endloop\nendfacet\n' for f in faces) + 'endsolid tetra\n'
    binary = b'tetra'.ljust(80, b'\0') + struct.pack('<I', len(faces)) + b''.join(struct.pack('<12fH', 0, 0, 0, *(v for i in f for v in vertices[i]), 0) for f in faces)
    geometry = b''.join(struct.pack('<3f', *vertices[i]) for f in faces for i in f)
    doc = {"asset": {"version": "2.0"}, "buffers": [{"byteLength":len(geometry), "uri":"data:application/octet-stream;base64,"+base64.b64encode(geometry).decode()}], "bufferViews":[{"buffer":0,"byteLength":len(geometry)}], "accessors":[{"bufferView":0,"componentType":5126,"count":12,"type":"VEC3","min":[0,0,0],"max":[1,1,1]}], "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}], "nodes":[{"mesh":0}], "scenes":[{"nodes":[0]}], "scene":0}
    gltf = json.dumps(doc).encode()
    del doc['buffers'][0]['uri']
    json_chunk = json.dumps(doc).encode()
    json_chunk += b' ' * (-len(json_chunk) % 4)
    glb = struct.pack('<III', 0x46546c67, 2, 28+len(json_chunk)+len(geometry)) + struct.pack('<II',len(json_chunk),0x4e4f534a) + json_chunk + struct.pack('<II',len(geometry),0x004e4942) + geometry
    for name, data in [('tetra.obj', obj.encode()), ('tetra.ply', ply.encode()), ('ascii.stl', stl.encode()), ('binary.stl', binary), ('tetra.gltf', gltf), ('tetra.glb', glb)]:
        sample = root / name
        sample.write_bytes(data)
        for size in (128, 256):
            render(sample, size)
        print(name, 'passed')
    render(root / 'tetra.obj', 8000)
    cube = root / 'cube.obj'
    cube.write_text("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\nf 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf 4 8 5 1\n")
    render(cube, 512)
    # Assimp 6.0.6 normalizes the OBJ token "nan" to finite zero; other versions
    # reject it. Either result is safe, but a returned image must pass all checks.
    permissive = root / 'ascii-nan.obj'
    permissive.write_bytes(b'v nan 0 0\nv 0 1 0\nv 1 0 0\nf 1 2 3\n')
    rendered = render(permissive, allow_rejection=True)
    print(permissive.name, 'validated image' if rendered else 'safely rejected')
    failures = [('not-model.obj', b'not a model'), ('empty.obj', b''),
                ('invalid-index.obj', b'v 0 0 0\nf 1 2 9999999\n'),
                ('truncated.stl', binary[:100])]
    # Binary coordinates bypass permissive ASCII conversion and must never reach
    # the rasterizer as NaN or infinity, regardless of the Assimp version.
    for name, value in [('nan', float('nan')), ('inf', float('inf')), ('negative-inf', -float('inf'))]:
        nonfinite = bytearray(binary)
        struct.pack_into('<f', nonfinite, 96, value)  # first triangle's first vertex x
        failures.append((f'binary-{name}.stl', nonfinite))
    for name, payload in failures:
        bad = root / name
        bad.write_bytes(payload)
        render(bad, expected_failure=True)
        print(name, 'rejected')
    doc['buffers'][0]['uri'] = str(root / 'external.bin')
    (root / 'external.bin').write_bytes(geometry)
    bad = root / 'external.gltf'
    bad.write_text(json.dumps(doc))
    render(bad, expected_failure=True)
    with bad.open('wb') as out:
        out.truncate(32 * 1024 * 1024 + 1)
    render(bad, expected_failure=True)
print('Model helper: six formats, scales, malformed, external-resource denial and oversized inputs passed under sandbox.')
