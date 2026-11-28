#!/usr/bin/env python3
"""Standalone TSan build/run. Execute only in the parent's authorized benchmark window."""
import argparse,hashlib,json,os,pathlib,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--window-authorized',action='store_true');p.add_argument('--repeat',type=int,default=3);a=p.parse_args()
if not a.window_authorized:raise SystemExit('Prepared only: parent must authorize an idle compile/run window first.')
root=pathlib.Path('/tmp/rummage-photo-library');out=pathlib.Path(__file__).parent
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
# This run validates current working-tree sources; source hashes recorded below.
cxx='/nix/store/ii6p6kc1brz3vycqg88q4zb4xd2dmqwp-gcc-wrapper-16.2.0/bin/g++'
flags=['-std=c++20','-O1','-g','-fno-omit-frame-pointer','-fsanitize=thread','-pthread','-fPIE','-pie','-I'+str(root/'src/core/include'),'-I/nix/store/mncfvp2l78ascq4qsd7f7zkmma1rz0wp-doctest-2.5.3/include']
env=os.environ.copy()
for key in ['LD_PRELOAD','ASAN_OPTIONS','LSAN_OPTIONS','UBSAN_OPTIONS','TSAN_OPTIONS','NIX_CFLAGS_COMPILE','NIX_LDFLAGS']:env.pop(key,None)
env.update(TSAN_OPTIONS='halt_on_error=1:exitcode=66:history_size=7',RUMMAGE_LOG='off')
sources=[str(root/x) for x in ['src/core/src/event_loop.cpp','src/core/src/worker_pool.cpp','src/core/src/log.cpp','src/core/src/memory.cpp','tests/worker_pool_test.cpp','tests/event_loop_test.cpp']]+[str(out/'main.cpp'),str(out/'stress.cpp')]
record=dict(head=head,working_tree_status=subprocess.check_output(['git','status','--short'],cwd=root,text=True),compiler=cxx,flags=flags,environment={k:env[k] for k in ['TSAN_OPTIONS','RUMMAGE_LOG']},source_sha256={s:hashlib.sha256(pathlib.Path(s).read_bytes()).hexdigest() for s in sources},steps=[])
def run(name,cmd,timeout=180):
 start=time.monotonic()
 with (out/(name+'.log')).open('w') as f:
  try:status=subprocess.run(cmd,cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
  except subprocess.TimeoutExpired:status='timeout'
 step=dict(name=name,command=cmd,exit=status,seconds=round(time.monotonic()-start,3));record['steps'].append(step)
 (out/'results.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(step),flush=True);return status,(out/(name+'.log')).read_text()
status,_=run('compile-control',[cxx,*flags,str(out/'race_control.cpp'),'-o',str(out/'race-control')])
if status:raise SystemExit('TSan control compilation failed; no pass claimed')
status,log=run('control',[str(out/'race-control')],30)
if status!=66 or 'WARNING: ThreadSanitizer: data race' not in log:raise SystemExit('TSan control did not detect the intentional race; inspect runtime failure, no pass claimed')
status,_=run('compile-core',[cxx,*flags,*sources,'-o',str(out/'core-tsan')],240)
if status:raise SystemExit('TSan core compilation failed; no pass claimed')
for i in range(a.repeat):
 status,log=run(f'core-{i+1}',[str(out/'core-tsan'),'--no-skip','--order-by=rand',f'--rand-seed={731+i}'],120)
 if status:raise SystemExit('Core sanitizer test failed; inspect log, no blanket pass claimed')
print('Targeted core TSan checks completed; inspect per-run test counts and logs.',flush=True)
