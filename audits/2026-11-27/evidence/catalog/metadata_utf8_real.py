#!/usr/bin/env python3
"""A valid image carrying long non-ASCII camera text; all files/cache/catalog disposable."""
import hashlib, json, os, sqlite3, struct, subprocess, sys, tempfile
from pathlib import Path
program=Path(sys.argv[1]); output=Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix='rummage-audit-metadata-') as temporary:
    root=Path(temporary);source=root/'source';source.mkdir()
    jpeg=Path('/tmp/rummage-photo-library/tests/data/image/blocks.jpg').read_bytes()
    camera=('é'*128).encode()+b'\0'
    tiff=b'II'+struct.pack('<HI',42,8)+struct.pack('<H',1)+struct.pack('<HHII',0x010f,2,len(camera),26)+struct.pack('<I',0)+camera
    exif=b'Exif\0\0'+tiff
    (source/'long-camera.jpg').write_bytes(jpeg[:2]+b'\xff\xe1'+struct.pack('>H',len(exif)+2)+exif+jpeg[2:])
    env=dict(os.environ)
    env.update({f'XDG_{name}_HOME':str(root/name.lower()) for name in ('CACHE','DATA','STATE','CONFIG')})
    catalog=root/'catalog/catalog.db'
    run=subprocess.run([str(program),str(catalog),str(source)],env=env,text=True,capture_output=True,timeout=30)
    report={'program':str(program),'sha256':hashlib.sha256(program.read_bytes()).hexdigest(),'returncode':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
    if catalog.exists():
        with sqlite3.connect(catalog) as db:
            report['photos']=db.execute('SELECT availability,metadata_signature FROM photos').fetchall()
            report['claims']=db.execute('SELECT count(*) FROM metadata_claims').fetchone()[0]
            report['outcomes']=db.execute('SELECT count(*) FROM outcomes').fetchone()[0]
    output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
