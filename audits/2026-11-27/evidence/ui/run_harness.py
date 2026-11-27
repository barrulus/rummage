import json,os,pathlib,subprocess,sys,time
root=pathlib.Path(__file__).parent
exe=sys.argv[1]; count=sys.argv[2];label=sys.argv[3]
env=os.environ.copy()
for key,folder in [('XDG_DATA_HOME','data'),('XDG_CONFIG_HOME','config'),('XDG_STATE_HOME','state'),('XDG_CACHE_HOME','cache')]:
 p=root/'runtime'/label/folder;p.mkdir(parents=True,exist_ok=True);env[key]=str(p)
env['AUDIT_FRAME_PATH']=str(root/(label+'.ppm'))
env.update(EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',RUMMAGE_LOG='off',XDG_DATA_DIRS='/nix/store/z3a2jiqszbcix6rqnknzvaxvp0ihh6fc-shared-mime-info-2.5.1/share')
samples=[];started=time.monotonic();timedout=False
with open(root/(label+'.csv'),'w') as out,open(root/(label+'.stderr'),'w') as err:
 p=subprocess.Popen([exe,count,str(root/'runtime'/label/'nav')],env=env,stdout=out,stderr=err)
 while p.poll() is None:
  sample={'seconds':time.monotonic()-started}
  try:
   for line in pathlib.Path(f'/proc/{p.pid}/smaps_rollup').read_text().splitlines():
    key,_,value=line.partition(':')
    if key in ('Rss','Pss','Private_Dirty'):sample[key+'_kib']=int(value.split()[0])
   samples.append(sample)
  except (FileNotFoundError,ProcessLookupError):pass
  if time.monotonic()-started>120:
   timedout=True;p.kill();p.wait();break
  try:p.wait(timeout=.25)
  except subprocess.TimeoutExpired:pass
elapsed=time.monotonic()-started
summary={'exit':p.returncode,'timeout':timedout,'wall_seconds':elapsed,'samples':samples,'peak_rss_kib':max((s.get('Rss_kib',0) for s in samples),default=0),'peak_pss_kib':max((s.get('Pss_kib',0) for s in samples),default=0)}
(root/(label+'.resources.json')).write_text(json.dumps(summary,indent=2))
print('exit',p.returncode,'seconds',round(elapsed,3),'peak_rss_kib',summary['peak_rss_kib'],'result',root/(label+'.csv'))
raise SystemExit(p.returncode)
