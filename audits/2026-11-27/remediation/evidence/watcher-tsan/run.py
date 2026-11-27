from pathlib import Path
import json,subprocess
out=Path('/tmp/rummage-photo-fixes-syntax/watcher-tsan'); root=Path('/tmp/rummage-photo-library')
command=['g++','-std=c++20','-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer','-pthread','-fPIE','-pie','-I'+str(root/'src/core/include'),'-I'+str(root/'src/fs/include'),'-I'+str(root/'tests'),str(root/'src/core/src/event_loop.cpp'),str(root/'src/core/src/worker_pool.cpp'),str(root/'src/core/src/log.cpp'),str(root/'src/fs/src/directory_watcher.cpp'),str(out/'test.cpp'),'-o',str(out/'test')]
r=subprocess.run(command,capture_output=True,text=True,timeout=180);(out/'compile.log').write_text(r.stdout+r.stderr);print('compile',r.returncode,flush=True)
if r.returncode: print(r.stderr);raise SystemExit(r.returncode)
for seed in [901,902,903]:
 cmd=[str(out/'test'),'--no-skip','--order-by=rand','--rand-seed='+str(seed)]
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=60);(out/(str(seed)+'.log')).write_text(r.stdout+r.stderr);print('run',seed,r.returncode,r.stdout[-400:],flush=True)
 if r.returncode: print(r.stderr);raise SystemExit(r.returncode)
