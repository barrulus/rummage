#!/usr/bin/env python3
"""Exercise the installed optional ABI and ensure diagnostics never initialize its catalog."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def exercise(module_path, root):
    module = ctypes.CDLL(str(module_path))
    module.rummage_photos_version.restype = ctypes.c_int
    assert module.rummage_photos_version() == 1, 'Incompatible photo module ABI'
    module.rummage_photos_request.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    module.rummage_photos_request.restype = ctypes.c_void_p
    module.rummage_photos_free.argtypes = [ctypes.c_void_p]
    module.rummage_photos_free.restype = None
    catalog = root / 'catalog/catalog.sqlite3'

    def request(**payload):
        result = module.rummage_photos_request(os.fsencode(catalog), json.dumps(payload).encode())
        assert result, 'The installed photo module returned no response'
        try:
            response = json.loads(ctypes.string_at(result))
        finally:
            module.rummage_photos_free(result)
        assert response.get('ok'), response
        return response

    assert not catalog.exists()
    assert request(op='query')['total'] == 0
    assert request(op='sources')['sources'] == []
    # Valid 1x1 uncompressed BMP; these are disposable fixtures, never personal originals.
    bitmap = (b'BM' + struct.pack('<IHHI', 58, 0, 0, 54)
              + struct.pack('<IiiHHIIiiII', 40, 1, 1, 1, 24, 0, 4, 0, 0, 0, 0)
              + b'\x00\x00\xff\x00')
    for folder in ('first', 'second'):
        source = root / folder
        source.mkdir()
        (source / 'same.bmp').write_bytes(bitmap)
        request(op='add_source', path=str(source), recursive=True)
    for _ in range(100):
        result = request(op='work', budget=8)
        for row in result.get('metadata', []):
            claim = request(op='metadata_claim', id=row['id'], signature=row['signature'])
            if claim['allowed']:
                request(op='metadata', id=row['id'], signature=row['signature'],
                        claim_token=claim['claim_token'], width=1, height=1)
        if not result.get('worked') and not result.get('metadata'):
            break
    else:
        raise AssertionError('Installed module did not complete bounded fixture discovery')
    rows = request(op='query')['photos']
    assert len(rows) == 2 and rows[0]['id'] != rows[1]['id'], rows
    photo_id = rows[0]['id']
    request(op='edit', ids=[photo_id], description='Voyage 日本', add_tags=['旅行'], rating=4,
            capture_date='2024-06-01T12:30:00')
    found = request(op='query', text='旅行')['photos']
    assert len(found) == 1 and found[0]['id'] == photo_id, found
    assert found[0]['date_origin'] == 'user', found
    source = Path(found[0]['path']).parent
    source.rename(source.with_name(source.name + '-offline'))
    offline = request(op='query', text='旅行')['photos']
    assert offline[0]['availability'] == 'offline', offline
    assert offline[0]['description'] == 'Voyage 日本'
    assert request(op='export')['data']['version'] == 1
    assert all(path.read_bytes() == bitmap for path in root.glob('*/same.bmp'))
    print('Installed photo ABI: separate identities, persistent edits and offline retention passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('--module', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rummage-installed-photos-') as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.pop('RUMMAGE_PHOTOS_LIBRARY', None)
        env.update(HOME=str(root / 'home'), XDG_DATA_HOME=str(root / 'data'),
                   XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
                   XDG_CACHE_HOME=str(root / 'cache'))
        config = root / 'settings.toml'
        for enabled in (True, False):
            config.write_text('[photos]\nenabled = ' + str(enabled).lower() + '\n')
            result = subprocess.run([str(args.application), '--config', str(config), '--about'],
                                    env=env, check=True, capture_output=True, text=True, timeout=30)
            assert 'Photo Library' in result.stdout, result.stdout
            if args.module:
                assert 'installed:' in result.stdout, result.stdout
            else:
                assert 'not installed' in result.stdout, result.stdout
            assert not (root / 'data/rummage/photos').exists(), 'Diagnostics initialized a catalog'
        # Nix's public wrapper intentionally pins its selected module. Exercise the actual
        # executable for the authoritative missing-override case without replacing the wrapper.
        binary = args.application.parent / '.rummage-wrapped'
        if not binary.is_file():
            binary = args.application
        missing = root / 'missing-photo-module.so'
        overridden = dict(env, RUMMAGE_PHOTOS_LIBRARY=str(missing))
        result = subprocess.run([str(binary), '--config', str(config), '--about'],
                                env=overridden, check=True, capture_output=True, text=True,
                                timeout=30)
        assert 'missing: ' + str(missing) in result.stdout, result.stdout
        assert not (root / 'data/rummage/photos').exists(), 'Missing add-on initialized a catalog'
        if args.module:
            exercise(args.module, root)
        print('Installed photo diagnostics: no catalog initialization, enabled or disabled')


if __name__ == '__main__':
    main()
