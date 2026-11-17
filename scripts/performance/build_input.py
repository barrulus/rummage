#!/usr/bin/env python3
"""Add monotonic send timestamps to Umbriel's virtual input test client.

Requires an Umbriel source/build tree with the virtual input protocols generated.
Only the temporary copy is changed. This client must connect to a dedicated
benchmark compositor, never the user's desktop.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--umbriel', type=Path, required=True)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
code = (a.umbriel / 'tests/harness/clients/pointer_client.cpp').read_text()
anchor = 'zwp_virtual_keyboard_v1_key(keyboard.protocol, nextTime(), key, WL_KEYBOARD_KEY_STATE_PRESSED);'
assert code.count(anchor) == 1
code = code.replace(anchor, '''std::printf("send %lld %u\\n", static_cast<long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()), key);
        std::fflush(stdout);
        ''' + anchor)
anchor = 'zwlr_virtual_pointer_v1_axis(pointer, nextTime(), axis, wl_fixed_from_double(delta));'
assert code.count(anchor) == 1
code = code.replace(anchor, '''std::printf("axis %lld\\n", static_cast<long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
        std::fflush(stdout);
        ''' + anchor)
source = a.out / 'input.cpp'
source.write_text(code)
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client', 'xkbcommon'], text=True).split()
objects = []
for protocol in ('virtual-keyboard-unstable-v1', 'wlr-virtual-pointer-unstable-v1'):
    obj = a.out / (protocol + '.o')
    subprocess.run(['cc', '-c', str(a.build / (protocol + '-protocol.c')), '-o', str(obj)] + flags, check=True)
    objects.append(str(obj))
subprocess.run(['c++', '-std=c++23', '-O2', '-I' + str(a.build), str(source), *objects,
                '-o', str(a.out / 'input')] + flags, check=True)
