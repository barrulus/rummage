#!/usr/bin/env python3
import pathlib,subprocess,os,json,time,re,sys,importlib.util,statistics,signal
root=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27');src=pathlib.Path('/tmp/rummage-photo-library');out=root/'root/live';out.mkdir(exist_ok=True)
runtime=pathlib.Path(sys.argv[1]);assert runtime!=pathlib.Path(os.environ.get('XDG_RUNTIME_DIR','/')) and runtime.is_relative_to(root)
spec=importlib.util.spec_from_file_location('metrics',src/'scripts/performance/run.py');metrics=importlib.util.module_from_spec(spec);spec.loader.exec_module(metrics)
exe=root/'build-release/rummage';env={k:v for k,v in os.environ.items() if k not in ('DISPLAY','WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS','UMBRIEL_SOCKET','RUMMAGE_STARTUP_NS','RUMMAGE_STARTUP_QUIT')};env.update(XDG_RUNTIME_DIR=str(runtime),WAYLAND_DISPLAY='wayland-0',DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={runtime}/no-system-bus')
for kind in ['CONFIG','CACHE','STATE','DATA']:
 p=out/kind.lower();p.mkdir(exist_ok=True);env[f'XDG_{kind}_HOME']=str(p)
config=out/'config.toml';config.write_text('[icon]\ntheme = "builtin"\nfollow_accent = false\n')
fixtures=root/'root/fixtures';small=fixtures/'files20';small.mkdir(exist_ok=True)
for i in range(20):(small/f'file-{i:02}.txt').write_text('fixture\n')
results={'scope':'Private headless hardware Wayland display; real unmodified Release executable; warm page caches; untouched desktop; no virtual input','startup':{},'idle':{},'renderer':None}
for label,folder in [('20',small),('10000',fixtures/'entries-10000'),('100000',fixtures/'entries-100000')]:
 samples=[]
 for trial in range(8):
  log=out/f'start-{label}-{trial}.log';trial_env=dict(env,RUMMAGE_STARTUP_NS=str(time.monotonic_ns()),RUMMAGE_STARTUP_QUIT='1');start=time.monotonic()
  with log.open('w') as f:
   p=subprocess.Popen([str(exe),'--config',str(config),str(folder)],env=trial_env,stdout=f,stderr=f,start_new_session=True)
   try:code=p.wait(timeout=30)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait();code='timeout30s'
  text=log.read_text();steps={k:float(v) for k,v in re.findall(r'startup: ([\w-]+) ([\d.]+) ms',text)}
  renderer=re.search(r'GL: (.+)',text)
  if renderer:results['renderer']=renderer.group(1)
  samples.append({'trial':trial,'warmup':trial==0,'exit':code,'steps_ms':steps,'process_wall_s':time.monotonic()-start})
 results['startup'][label]={'samples':samples,'folder_shown_ms':metrics.distribution([s['steps_ms']['folder-shown'] for s in samples[1:] if 'folder-shown' in s['steps_ms']])}
 log=out/f'idle-{label}.log'
 with log.open('w') as f:
  p=subprocess.Popen([str(exe),'--config',str(config),str(folder)],env=env,stdout=f,stderr=f,start_new_session=True)
  try:
   time.sleep(5)
   if p.poll() is not None:results['idle'][label]={'error':'exited','exit':p.returncode}
   else:
    samples=[]
    for i in range(3):samples.append({'memory':metrics.memory_tree(p.pid),'cpu':metrics.idle_cpu(p.pid,3)})
    results['idle'][label]={'samples':samples}
  finally:
   if p.poll() is None:
    os.killpg(p.pid,signal.SIGTERM)
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps({'renderer':results['renderer'],'startup':{k:v['folder_shown_ms'] for k,v in results['startup'].items()}}))
