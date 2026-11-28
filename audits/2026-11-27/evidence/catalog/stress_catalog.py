#!/usr/bin/env python3
"""Isolated synthetic catalog stress. No user data, network, GUI, or image decoding.
Run only when parent has released the timed benchmark slot; output includes wall/load context.
"""
import argparse, concurrent.futures, hashlib, json, os, platform, resource, sqlite3, statistics, tempfile, time
from pathlib import Path
from audit_support import seed_synthetic, Catalog

def timed(fn):
    begin=time.perf_counter(); value=fn(); return value,(time.perf_counter()-begin)*1000

def distribution(values):
    values=sorted(values)
    return {'samples':len(values),'p50_ms':statistics.median(values),'p95_ms':values[min(len(values)-1,int(len(values)*.95))],'max_ms':max(values),'total_ms':sum(values)}

def run(args):
    report={'revision':'1dd2368783d814e86268fd2c388ee41671c66ae8','module':str(args.module),'module_sha256':hashlib.sha256(args.module.read_bytes()).hexdigest(),'build_mode':args.build_mode,'scope':'synthetic catalog only; no decoding/UI measurement; warm filesystem cache','records':args.records,'machine':{'kernel':platform.release(),'cpu':next((l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name')),'unknown'),'cpu_count':os.cpu_count(),'load_start':os.getloadavg()},'workloads':{}}
    with tempfile.TemporaryDirectory(prefix='rummage-audit-stress-') as temporary:
        root=Path(temporary); source=root/'source'
        c,setup=timed(lambda:seed_synthetic(args.module,root/'data/catalog.db',source,args.records,job_history=1000));report['setup_ms']=setup
        def measure(name,fn,samples=10):
            samples_ms=[];last=None
            for _ in range(samples):last,elapsed=timed(fn);samples_ms.append(elapsed)
            report['workloads'][name]=distribution(samples_ms);print(name,report['workloads'][name],flush=True)
            return last
        for name,query in {'first_page':{},'mid_offset':{'offset':args.records//2},'last_offset':{'offset':args.records-200},'text':{'text':'coast'},'rating':{'rating_min':4,'sort':'rating'},'capture_date':{'date_from':'2024-06-01','date_to':'2024-08-31','sort':'date'},'size':{'sort':'size'},'modified':{'sort':'modified'},'location':{'sort':'location'}}.items():
            result=measure(name,lambda q=query:c.call({'op':'query','limit':200,**q}),args.samples);assert len(result['photos'])<=200
        times=[];ids=set()
        for offset in range(0,args.records,200):
            page,elapsed=timed(lambda o=offset:c.call({'op':'query','offset':o,'limit':200})); times.append(elapsed)
            pageids={p['id'] for p in page['photos']};assert not ids.intersection(pageids);ids.update(pageids)
        assert len(ids)==args.records
        report['workloads']['continuous_append_pages']=distribution(times);print('append',len(ids),flush=True)
        measure('jobs_1000_history',lambda:c.call({'op':'jobs'}),args.samples)
        for requested in (10000,30000,100000):
            count=min(requested,args.records)
            def reload_prefix():
                rows=[]
                for offset in range(0,count,1000):
                    rows.extend(c.call({'op':'query','offset':offset,'limit':min(1000,count-offset)})['photos'])
                assert len(rows)==count
                return len(rows)
            measure(f'controller_prefix_{count}',reload_prefix,1)
            if count==args.records:break
        # Append real one-byte originals directly with their true stat identities. They
        # exercise metadata persistence/claims only; supplied dimensions are synthetic.
        with sqlite3.connect(c.path) as db:
            sid=db.execute('SELECT id FROM sources').fetchone()[0];job=db.execute('SELECT max(id) FROM jobs').fetchone()[0]
            for i in range(256):
                file=source/f'real-{i:04}.jpg';file.write_bytes(b'x');st=file.stat(); sec,nano=divmod(st.st_mtime_ns,10**9)
                db.execute('INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,seen_job) VALUES(?,?,?,?,?,?,?,?,?)',(sid,file.name,file.name,st.st_dev,st.st_ino,st.st_size,sec,nano,job))
        pending=measure('metadata_pending_tail',lambda:c.call({'op':'metadata_pending','limit':4}),args.samples)
        claim_times=[];metadata_times=[]
        for _ in range(64):
            pending=c.call({'op':'metadata_pending','limit':4})['metadata']
            for row in pending:
                claim,elapsed=timed(lambda:c.call({'op':'metadata_claim','id':row['id'],'signature':row['signature'],'owner':str(os.getpid())}));assert claim['allowed'];claim_times.append(elapsed)
                result,elapsed=timed(lambda:c.call({'op':'metadata','id':row['id'],'signature':row['signature'],'claim_token':claim['claim_token'],'width':100,'height':80}));assert not result.get('stale');metadata_times.append(elapsed)
        assert not c.call({'op':'metadata_pending'})['metadata']
        report['workloads']['metadata_claim']=distribution(claim_times);report['workloads']['metadata_persist']=distribution(metadata_times)
        exported,elapsed=timed(lambda:c.call({'op':'export'})['data']);report['workloads']['export']={'ms':elapsed,'photo_count':len(exported['photos']),'json_bytes':len(json.dumps(exported).encode())};print('export',report['workloads']['export'],flush=True)
        restored,elapsed=timed(lambda:c.call({'op':'restore','data':exported}));assert restored['restored']==args.records+256 and not restored['missing'] and not restored['ambiguous'];report['workloads']['restore_existing_matches']={'ms':elapsed,'restored':restored['restored']};print('restore',elapsed,flush=True)
        # Fresh catalog restore uses only the 256 real files; synthetic path records have
        # no originals and therefore cannot establish a fresh-catalog identity match.
        small={**exported,'photos':[p for p in exported['photos'] if p['filename'].startswith('real-')]}
        fresh=Catalog(args.module,root/'fresh/catalog.db'); restored=measure('restore_fresh_256_real_identities',lambda:fresh.call({'op':'restore','data':small}),1);assert restored['restored']==256
        requests=[{'op':'query','limit':200,'offset':i*200%args.records} if i%3 else {'op':'edit','ids':[1+i%args.records],'rating':i%6} for i in range(120)]
        begin=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: results=list(pool.map(lambda r:c.call(r,False),requests))
        report['workloads']['concurrent_read_write']={'ms':(time.perf_counter()-begin)*1000,'requests':len(results),'errors':[r for r in results if not r['ok']]}
        with sqlite3.connect(c.path) as db:report['integrity']=db.execute('PRAGMA integrity_check').fetchone()[0];report['foreign_key_errors']=db.execute('PRAGMA foreign_key_check').fetchall()
        report['database_bytes']=c.path.stat().st_size
    report['peak_rss_kib']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss;report['machine']['load_end']=os.getloadavg();return report
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('module',type=Path);p.add_argument('--records',type=int,default=100000);p.add_argument('--samples',type=int,default=10);p.add_argument('--build-mode',required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    result=run(args);args.output.write_text(json.dumps(result,indent=2)+'\n');print(args.output,flush=True)
