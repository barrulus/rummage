import sys,json,os,time,signal,subprocess,hashlib
from pathlib import Path
root=Path('/tmp/rummage-final-sshfs-check'); mount=root/'mount-default'; mount.mkdir();state=root/'default-transport-state';state.mkdir()
sshargs=['sshfs','-f','-p','22679','-o',f'IdentityFile={root}/client_key,IdentitiesOnly=yes,UserKnownHostsFile={root}/known_hosts,StrictHostKeyChecking=yes,BatchMode=yes,reconnect,ServerAliveInterval=1,ServerAliveCountMax=2,ConnectTimeout=15','barrulus@127.0.0.1:'+str(root/'source'),str(mount)]
log=(root/'default-transport-sshfs.log').open('w');proc=subprocess.Popen(sshargs,stdout=log,stderr=log,start_new_session=True)
sys.path.insert(0,str(root));sys.argv=['check',str(state/'catalog.sqlite3')]
from catalog_call import call
def drain():
 for _ in range(1000):
  r=call({'op':'work','budget':256})
  for m in r.get('metadata',[]):call({'op':'metadata','id':m['id'],'signature':m['signature'],'width':320,'height':240})
  if not r.get('worked') and not r.get('metadata'):return
 raise RuntimeError('work did not drain')
def stats(rows):
 return {p['filename']:{'device':(s:=Path(p['path']).stat()).st_dev,'inode':s.st_ino,'size':s.st_size,'mtime_ns':s.st_mtime_ns} for p in rows}
def changed(a,b):return [name for name in a if a[name]!=b[name]]
try:
 end=time.monotonic()+15
 while not os.path.ismount(mount):
  assert proc.poll() is None,'sshfs exited at startup'
  assert time.monotonic()<end,'mount startup timeout'
  time.sleep(.05)
 assert len(list(mount.glob('folder-*/*.png')))==300
 call({'op':'add_source','path':str(mount)});drain();before=call({'op':'query','limit':1000})['photos'];assert len(before)==300
 call({'op':'create_album','name':'Default cache retained','ids':[p['id'] for p in before[:5]]})
 cat={p['filename']:{'device':p['device'],'inode':p['inode'],'size':p['size'],'mtime_ns':p['mtime_ns']} for p in before}
 first=stats(before);time.sleep(30);second=stats(before)
 # Kill only SSH sessions belonging to this fixture's private localhost listener.
 server=int((root/'server-pid').read_text());rows=subprocess.check_output(['ps','-eo','pid=,ppid='],text=True);pairs=[tuple(map(int,l.split())) for l in rows.splitlines()];pids={server}
 while True:
  more={p for p,pp in pairs if pp in pids}
  if more<=pids:break
  pids|=more
 pids.remove(server)
 start=time.monotonic_ns()
 for p in sorted(pids,reverse=True):
  try:os.kill(p,signal.SIGKILL)
  except ProcessLookupError:pass
 marker='default-cache-outage-marker.txt';(root/'source'/marker).write_text('never looked up before connection loss')
 check=subprocess.run(['stat',str(mount/marker)],capture_output=True,text=True,timeout=30)
 assert check.returncode==0,check.stderr
 elapsed=(time.monotonic_ns()-start)/1e9
 assert proc.poll() is None,'sshfs died after disconnect'
 time.sleep(30);third=stats(before)
 assert proc.poll() is None and os.path.ismount(mount)
 preview=call({'op':'relink_source','source_id':1,'path':str(mount)})
 call({'op':'refresh','source_id':1});drain()
 result={'options':sshargs,'session_pids_killed':sorted(pids),'reconnect_and_fresh_stat_seconds':elapsed,'catalog_count_before':len(before),'catalog_to_initial_changed':changed(cat,first),'idle_30s_changed':changed(first,second),'outage_plus_30s_changed':changed(second,third),'first':first,'before_outage':second,'after_outage':third,'relink_preview':{k:len(v) if isinstance(v,list) else v for k,v in preview.items()},'after_rescan':{'active_photos':call({'op':'query'})['total'],'visible_album_members':call({'op':'query','album_id':1})['total'],'retained_album_members':call({'op':'query','album_id':1,'retained_only':True})['total']}}
 dest=Path('audits/2026-11-27/final-validation/sshfs/default-cache-confirmed.json');dest.write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps({k:len(v) if isinstance(v,list) else v for k,v in result.items() if k not in ('first','before_outage','after_outage','options')},indent=2),flush=True)
finally:
 subprocess.run(['fusermount3','-u',str(mount)],capture_output=True)
 if proc.poll() is None:proc.terminate()
 try:proc.wait(timeout=5)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
