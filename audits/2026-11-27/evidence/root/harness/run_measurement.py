#!/usr/bin/env python3
import sys,os,json,time,resource,subprocess,pathlib,hashlib,datetime
out=pathlib.Path(sys.argv[1]); args=sys.argv[2:]; out.parent.mkdir(parents=True,exist_ok=True)
start=time.perf_counter(); load_before=pathlib.Path('/proc/loadavg').read_text().strip()
with out.with_suffix('.log').open('w') as log:
    try:
        result=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=300)
        status=result.returncode
    except subprocess.TimeoutExpired:
        status='timeout300s'
usage=resource.getrusage(resource.RUSAGE_CHILDREN)
record={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'command':args,'exit':status,'wall_seconds':time.perf_counter()-start,'user_seconds':usage.ru_utime,'system_seconds':usage.ru_stime,'child_maxrss_KiB':usage.ru_maxrss,'load_before':load_before,'load_after':pathlib.Path('/proc/loadavg').read_text().strip(),'note':'Warm filesystem caches; shared host, no competing audit benchmark/build during timing. maxrss is child rusage, not aggregate process-tree memory.'}
out.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
sys.exit(0 if status==0 else 1)
