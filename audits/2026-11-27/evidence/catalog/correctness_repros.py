#!/usr/bin/env python3
import argparse, concurrent.futures, hashlib, json, os, sqlite3, tempfile, time
from pathlib import Path
from audit_support import Catalog

def run(module):
    report={'revision':'1dd2368783d814e86268fd2c388ee41671c66ae8','module':str(module),'sha256':hashlib.sha256(module.read_bytes()).hexdigest(),'cases':{}}
    with tempfile.TemporaryDirectory(prefix='rummage-audit-small-') as temporary:
        root=Path(temporary)
        # Existing backup represents an interrupted upgrade, without damaging the backup.
        c=Catalog(module,root/'upgrade/catalog.db'); c.call({'op':'create_album','name':'Retained'})
        with sqlite3.connect(c.path) as db:
            db.executescript('DROP TABLE source_directories; DROP TABLE background_settings; ALTER TABLE jobs DROP COLUMN refresh_again; PRAGMA user_version=1;')
            saved=sqlite3.connect(str(c.path)+'.before-schema-2.backup'); db.backup(saved); saved.close()
        replies=[c.call({'op':'albums'},False) for _ in range(2)]
        report['cases']['interrupted_upgrade_backup_collision']={'reproduced':all(not r['ok'] for r in replies),'replies':replies,'backup_preserved':Path(str(c.path)+'.before-schema-2.backup').exists()}
        # Tiny real originals; synthetic metadata only (no decoder claims).
        src=root/'source';src.mkdir(); original=src/'one.jpg';original.write_bytes(b'fixture')
        c=Catalog(module,root/'normal/catalog.db'); c.call({'op':'add_source','path':str(src)}); c.drain()
        row=c.call({'op':'query'})['photos'][0]; pid=row['id']
        c.call({'op':'edit','ids':[pid],'description':'backup value','add_tags':['backup']})
        backup=c.call({'op':'export'})['data']
        c.call({'op':'edit','ids':[pid],'description':'later value','add_tags':['later']})
        restored=c.call({'op':'restore','data':backup}); after=c.call({'op':'query'})['photos'][0]
        report['cases']['restore_merges_tags']={'reproduced':after['tags']!=backup['photos'][0]['tags'],'exported_tags':backup['photos'][0]['tags'],'restored_tags':after['tags'],'description':after['description'],'result':restored}
        # Export includes accepted source exclusions; malformed restore must not create unusable source.
        malformed=json.loads(json.dumps(backup)); malformed['sources'][0]['exclusions']=[7]
        d=Catalog(module,root/'malformed/catalog.db'); result=d.call({'op':'restore','data':malformed},False)
        d.call({'op':'refresh'}); work=d.call({'op':'work'},False)
        retry=d.call({'op':'add_source','path':str(src),'exclusions':[]});retry_work=d.call({'op':'work'},False)
        report['cases']['restore_invalid_exclusions']={'reproduced':result['ok'] and not work['ok'],'restore':result,'work':work,'readd_source_exclusions':retry['source']['exclusions'],'retry_work':retry_work}
        # POSIX mtime can exceed signed nanosecond range on filesystems with extended timestamps.
        far=root/'far';far.mkdir(); f=far/'future.jpg';f.write_bytes(b'future')
        try:
            requested=10_000_000_000_000_000_000;os.utime(f,ns=(requested,requested));actual=f.stat().st_mtime_ns
            t=Catalog(module,root/'far-catalog/catalog.db');t.call({'op':'add_source','path':str(far)});t.call({'op':'work'})
            queried=t.call({'op':'query'})['photos'][0]
            report['cases']['timestamp_overflow']={'reproduced':actual>2**63-1 and queried['mtime_ns']<0,'stat_mtime_ns':actual,'catalog_mtime_ns':queried['mtime_ns']}
        except Exception as exc: report['cases']['timestamp_overflow']={'error':str(exc)}
        # Shared read/write workload; the fresh-init race is separately covered by existing suite.
        requests=[]
        for i in range(40): requests.extend([{'op':'query','limit':1},{'op':'jobs'},{'op':'edit','ids':[pid],'rating':i%6},{'op':'background_state'}])
        begin=time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: responses=list(pool.map(lambda r:c.call(r,False),requests))
        with sqlite3.connect(c.path) as db: integrity=db.execute('PRAGMA integrity_check').fetchone()[0]
        report['cases']['concurrent_requests']={'requests':len(responses),'errors':[r for r in responses if not r['ok']],'integrity':integrity,'elapsed_s':time.monotonic()-begin}
    return report
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('module',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=run(a.module);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
