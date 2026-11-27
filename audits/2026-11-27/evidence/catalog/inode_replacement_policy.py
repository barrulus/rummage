#!/usr/bin/env python3
"""Simulates a prior synthetic inode changing across a refresh; no mount is accessed.
The SQLite fixture edit models unstable inode numbers, NOT an observed local filesystem event.
"""
import hashlib,json,sqlite3,sys,tempfile
from pathlib import Path
from audit_support import Catalog
with tempfile.TemporaryDirectory(prefix='rummage-audit-inode-') as temporary:
    root=Path(temporary);source=root/'source';source.mkdir();file=source/'unchanged.jpg';file.write_bytes(b'unchanged fixture bytes')
    before=hashlib.sha256(file.read_bytes()).hexdigest();c=Catalog(sys.argv[1],root/'catalog/catalog.db')
    c.call({'op':'add_source','path':str(source)});c.drain();photo=c.call({'op':'query'})['photos'][0]
    c.call({'op':'edit','ids':[photo['id']],'description':'Original annotation','add_tags':['remember']})
    album=c.call({'op':'create_album','name':'Original album','ids':[photo['id']]})
    album_id=c.call({'op':'albums'})['albums'][0]['id']
    with sqlite3.connect(c.path) as db:db.execute('UPDATE photos SET inode=inode+1000000 WHERE id=?',(photo['id'],))
    c.call({'op':'refresh'});c.drain()
    report={'scope':'synthetic prior-inode mismatch; not a real SSHFS measurement','old_id':photo['id'],'unchanged_original_bytes':hashlib.sha256(file.read_bytes()).hexdigest()==before,'visible':c.call({'op':'query'})['photos'],'album_listing':c.call({'op':'albums'})['albums'],'album_results':c.call({'op':'query','album_id':album_id})['total'],'retained_history':c.call({'op':'query','include_inactive':True})['photos']}
    Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('visible','retained_history')},indent=2));print('visible id,description:',[(p['id'],p['description']) for p in report['visible']]);print('retained id,state,description:',[(p['id'],p['availability'],p['description']) for p in report['retained_history']])
