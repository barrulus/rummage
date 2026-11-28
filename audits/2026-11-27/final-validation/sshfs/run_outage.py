import os,sys,json,time,signal,subprocess
from pathlib import Path
root=Path('/tmp/rummage-final-sshfs-check'); kind=sys.argv[2] if len(sys.argv)>2 else 'scroll'; out=root/('ui-'+kind); evidence=Path('/tmp/rummage-photo-library/audits/2026-11-27/final-validation/sshfs')
args=['python3','scripts/performance/photo_scroll.py','--runtime',sys.argv[1],'--catalog',str(root/'state/catalog.sqlite3'),'--executable',str(root/'probe/rummage'),'--out',str(out),'--case','large-three','--steps','90','--preload-all']
if kind=='refresh': args += ['--refresh-passes','3','--refresh-scroll']
proc=subprocess.Popen(args)
trace=out/'large-three/trace.txt'; events=[]; stopped=[]
def event(name,**kw):
 x={'event':name,'monotonic_ns':time.monotonic_ns(),**kw}; events.append(x); print(json.dumps(x),flush=True)
def descendants(pid):
 rows=subprocess.check_output(['ps','-eo','pid=,ppid='],text=True)
 pairs=[tuple(map(int,line.split())) for line in rows.splitlines()]
 result={pid}
 while True:
  more={p for p,pp in pairs if pp in result}
  if more<=result:return sorted(result)
  result|=more
try:
 deadline=time.monotonic()+40
 while True:
  if proc.poll() is not None:raise RuntimeError('UI exited before outage')
  keys=[l.split() for l in trace.read_text().splitlines() if l.startswith('key ')] if trace.exists() else []
  if len(keys)>=20:break
  if time.monotonic()>deadline:raise TimeoutError('UI startup')
  time.sleep(.04)
 server=int((root/'server-pid').read_text()); stopped=descendants(server)
 for p in stopped:os.kill(p,signal.SIGSTOP)
 event('transport_paused',pids=stopped)
 # This is an uncached attribute lookup through the real FUSE mount.
 check=subprocess.Popen(['stat',str(root/'mount/folder-2/photo-0299.png')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 time.sleep(8)
 event('during_outage',stat_still_waiting=check.poll() is None,ui_alive=proc.poll() is None)
 for p in stopped:
  try:os.kill(p,signal.SIGCONT)
  except ProcessLookupError:pass
 stopped=[]; event('transport_resumed')
 stdout,stderr=check.communicate(timeout=30)
 event('stat_completed',exit_code=check.returncode,stdout=stdout,stderr=stderr)
 proc.wait(timeout=140)
 event('ui_completed',exit_code=proc.returncode)
finally:
 for p in stopped:
  try:os.kill(p,signal.SIGCONT)
  except ProcessLookupError:pass
 if proc.poll() is None:proc.terminate();proc.wait(timeout=15)
 (evidence/('transport-events-'+kind+'.json')).write_text(json.dumps(events,indent=2)+'\n')
