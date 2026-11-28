#!/usr/bin/env python3
import pathlib,subprocess,os,json,time,signal
root=pathlib.Path('/tmp/rummage-photo-library');out=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/root/mutations')
allfiles=list((root/'tests/data').rglob('*'))
def seeds(suffixes):return [str(p) for p in allfiles if p.is_file() and p.suffix.lower() in suffixes and p.stat().st_size<=1024*1024][:100]
cases={'text':(50000,seeds({'.txt','.c','.cpp','.json','.md','.toml','.yaml'})), 'tiff':(50000,seeds({'.tif','.tiff','.dng'})), 'archive':(50000,seeds({'.zip','.tar','.gz'})), 'picture':(20000,seeds({'.jpg','.jpeg','.png','.gif','.webp','.heic','.jxl','.tif'})), 'svg':(20000,seeds({'.svg'}))}
results=[]
for name,(count,files) in cases.items():
 cmd=[str(out/('mutate-'+name)),str(count),str(out/(name+'-last-input.bin')),*files]
 start=time.monotonic();env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 with (out/(name+'.log')).open('w') as log:
  p=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,start_new_session=True);cause=None;peak=0
  while p.poll() is None:
   try:
    status=pathlib.Path(f'/proc/{p.pid}/status').read_text();rss=int(next(x.split()[1] for x in status.splitlines() if x.startswith('VmRSS:')));peak=max(peak,rss)
   except (FileNotFoundError,StopIteration):pass
   if peak>2*1024*1024:cause='RSS exceeds 2GiB'
   if time.monotonic()-start>180:cause='timeout180s'
   if cause:os.killpg(p.pid,signal.SIGKILL);p.wait();break
   time.sleep(.1)
 result={'target':name,'requested_iterations':count,'seeds':len(files),'exit':p.returncode,'stopped_by':cause,'wall_seconds':time.monotonic()-start,'sampled_peak_rss_KiB':peak,'command':cmd,'note':'Deterministic mutation smoke using existing fuzz entry points; no coverage guidance; uninstrumented system libraries remain outside sanitizer coverage.'}
 results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(result),flush=True)
