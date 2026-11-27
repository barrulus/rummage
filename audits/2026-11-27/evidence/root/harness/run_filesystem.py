import pathlib,subprocess,sys,json,time,os,hashlib,shutil
root=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27');out=root/'root/filesystem';out.mkdir(exist_ok=True);fixtures=root/'root/fixtures';build=root/'build-release';runner=root/'root/harness/run_measurement.py'
for count in [1000,10000,100000]:
 subprocess.run([sys.executable,str(runner),str(out/f'directory-{count}'),str(build/'rummage-bench-dir'),'--runs','7',str(fixtures/f'entries-{count}')],check=True)
subprocess.run([sys.executable,str(runner),str(out/'find-100000'),str(build/'rummage-bench-find'),'--runs','3',str(fixtures/'entries-100000'),'IMG','Session','not-present-fixture'],check=True)
source=fixtures/'copy-source'
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()
expected={p.name:sha(p) for p in source.iterdir()};records=[]
for program in ['rummage','cp']:
 for trial in range(3):
  dest=fixtures/f'copy-{program}-{trial}';dest.mkdir(exist_ok=False)
  cmd=[str(build/'rummage-transfer'),str(source),str(dest)] if program=='rummage' else [shutil.which('cp'),'-a',str(source)+'/.',str(dest)]
  start=time.perf_counter()
  with (out/f'copy-{program}-{trial}.log').open('w') as log:p=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=120)
  copy_end=time.perf_counter()
  for f in dest.iterdir():
   fd=os.open(f,os.O_RDONLY);os.fsync(fd);os.close(fd)
  fd=os.open(dest,os.O_RDONLY);os.fsync(fd);os.close(fd);end=time.perf_counter()
  actual={p.name:sha(p) for p in dest.iterdir()};assert actual==expected
  records.append({'program':program,'trial':trial,'exit':p.returncode,'files':len(actual),'bytes':sum(p.stat().st_size for p in source.iterdir()),'copy_seconds':copy_end-start,'synced_seconds':end-start,'verified_all_sha256':True,'command':cmd})
  assert dest.is_relative_to(fixtures);shutil.rmtree(dest)
(out/'copy-results.json').write_text(json.dumps(records,indent=2)+'\n');print(json.dumps(records))
