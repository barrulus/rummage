"""Prepare generated PNGs and private localhost SSH keys; never reads user media.

The adjacent reproduction drivers use this fixed root. Move the saved /tmp results
aside before another run, or replace the root consistently in all drivers.
"""
from pathlib import Path
import struct, zlib, subprocess
root = Path('/tmp/rummage-final-sshfs-check')
root.mkdir(exist_ok=False)
for kind in ('host', 'client'):
    subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(root / (kind + '_key'))], check=True)
source = root / 'source'
source.mkdir()
def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xffffffff)
for i in range(300):
    folder = source / f'folder-{i//100}'
    folder.mkdir(exist_ok=True)
    rgb = bytes([i % 256, (i*3) % 256, (i*7) % 256])
    raw = b''.join(b'\0' + rgb*320 for _ in range(240))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 320, 240, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    (folder / f'photo-{i:04}.png').write_bytes(png)
(root / 'mount').mkdir()
(root / 'state').mkdir()
(root / 'sshd_config').write_text(f'''Port 22679
ListenAddress 127.0.0.1
HostKey {root}/host_key
PidFile {root}/sshd.pid
AuthorizedKeysFile {root}/client_key.pub
StrictModes no
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
Subsystem sftp internal-sftp
LogLevel VERBOSE
AllowUsers barrulus
''')
with (root / 'sshd.log').open('w') as log:
    proc = subprocess.Popen(['/run/current-system/sw/bin/sshd', '-D', '-e', '-f', str(root / 'sshd_config')], stdout=log, stderr=log, start_new_session=True)
(root / 'server-pid').write_text(str(proc.pid))
print('Private sshd PID:', proc.pid)
